#include "ui/UiLayout.hpp"
#include <cmath>
#include "ui/UiKit.hpp"
#include "ui/ImGuiBridge.hpp"
#include "audio/SoundRequests.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "core/Localization.hpp"
#include "graphics/RenderUtility.hpp"
#include "graphics/SpriteRegistry.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>

namespace OpenArtOfWar
{
namespace ui
{

namespace
{

struct Theme
{
    SDL_Color panel, raised, sunken, border, borderStrong, hover, accent, accentDeep, accentText;
};

Theme g_themes[3] = {
    // neutral (menus — warm charcoal-bronze matching dialog_items chrome)
    {{23, 19, 16, 246},
     {35, 28, 23, 255},
     {16, 13, 10, 255},
     {90, 66, 48, 255},
     {140, 102, 66, 255},
     {51, 40, 31, 255},
     {232, 176, 74, 255},
     {120, 84, 30, 255},
     {24, 18, 8, 255}},
    // resistance
    {{26, 18, 17, 246},
     {38, 26, 24, 255},
     {17, 12, 11, 255},
     {82, 52, 46, 255},
     {124, 78, 68, 255},
     {52, 34, 31, 255},
     {228, 98, 66, 255},
     {116, 40, 26, 255},
     {26, 10, 6, 255}},
    // confederation
    {{15, 21, 31, 246},
     {22, 31, 46, 255},
     {10, 14, 22, 255},
     {44, 62, 90, 255},
     {70, 98, 136, 255},
     {29, 41, 60, 255},
     {88, 166, 238, 255},
     {28, 70, 122, 255},
     {6, 14, 26, 255}},
};

SDL_Color ReadColor(const YAML::Node &n, const char *key, SDL_Color fallback)
{
    try
    {
        if (n && n.IsMap() && n[key])
            return RenderUtility::ParseHexColor(n[key].as<std::string>(), fallback);
    }
    catch (...)
    {}
    return fallback;
}

} // namespace

bool LoadTheme(std::string_view yamlText)
{
    try
    {
        const YAML::Node root = YAML::Load(std::string(yamlText));
        if (!root.IsMap())
            return false;
        const char *const names[3] = {"neutral", "resistance", "confederation"};
        for (int i = 0; i < 3; ++i)
        {
            const YAML::Node n = root["themes"] ? root["themes"][names[i]] : YAML::Node();
            Theme &t = g_themes[i];
            t.panel = ReadColor(n, "panel", t.panel);
            t.raised = ReadColor(n, "panel_raised", t.raised);
            t.sunken = ReadColor(n, "panel_sunken", t.sunken);
            t.border = ReadColor(n, "border", t.border);
            t.borderStrong = ReadColor(n, "border_strong", t.borderStrong);
            t.hover = ReadColor(n, "hover", t.hover);
            t.accent = ReadColor(n, "accent", t.accent);
            t.accentDeep = ReadColor(n, "accent_deep", t.accentDeep);
            t.accentText = ReadColor(n, "accent_text", t.accentText);
        }
        const YAML::Node c = root["colors"];
        color::Backdrop = ReadColor(c, "backdrop", color::Backdrop);
        color::Scrim = ReadColor(c, "scrim", color::Scrim);
        color::Text = ReadColor(c, "text", color::Text);
        color::TextDim = ReadColor(c, "text_dim", color::TextDim);
        color::TextMuted = ReadColor(c, "text_muted", color::TextMuted);
        color::Danger = ReadColor(c, "danger", color::Danger);
        color::DangerDeep = ReadColor(c, "danger_deep", color::DangerDeep);
        color::Ok = ReadColor(c, "ok", color::Ok);
        color::Info = ReadColor(c, "info", color::Info);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void ApplyTheme(Faction faction, const SDL_Color *army)
{
    Theme t = faction == Faction::Resistance      ? g_themes[1]
              : faction == Faction::Confederation ? g_themes[2]
                                                  : g_themes[0];
    if (army)
    {
        // The army color becomes the accent; panels, borders and hover lean slightly towards it
        const SDL_Color a = *army;
        const SDL_Color black {0, 0, 0, 255};
        const float luma = 0.299f * a.r + 0.587f * a.g + 0.114f * a.b;
        t.accent = {a.r, a.g, a.b, 255};
        t.accentDeep = Mix(a, black, 0.55f);
        t.accentDeep.a = 255;
        t.accentText = luma > 140.0f ? SDL_Color {20, 16, 10, 255} : SDL_Color {245, 245, 245, 255};
        t.panel = Mix(t.panel, a, 0.07f);
        t.raised = Mix(t.raised, a, 0.08f);
        t.sunken = Mix(t.sunken, a, 0.05f);
        t.border = Mix(t.border, a, 0.30f);
        t.borderStrong = Mix(t.borderStrong, a, 0.45f);
        t.hover = Mix(t.hover, a, 0.14f);
        t.panel.a = 246;
    }
    color::Panel = t.panel;
    color::PanelRaised = t.raised;
    color::PanelSunken = t.sunken;
    color::Border = t.border;
    color::BorderStrong = t.borderStrong;
    color::Hover = t.hover;
    color::Accent = t.accent;
    color::AccentDeep = t.accentDeep;
    color::AccentText = t.accentText;
}

SDL_Color Mix(SDL_Color a, SDL_Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto lerp = [t](Uint8 x, Uint8 y) {
        return static_cast<Uint8>(x + (y - x) * t);
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

void FillVerticalGradient(Renderer &r, const UiRect &rect, SDL_Color top, SDL_Color bottom)
{
    if (rect.h <= 0 || rect.w <= 0)
        return;
    for (int i = 0; i < rect.h; ++i)
    {
        float t = (rect.h > 1) ? static_cast<float>(i) / static_cast<float>(rect.h - 1) : 0.0f;
        SDL_Color c = Mix(top, bottom, t);
        r.DrawLineScreen(rect.x, rect.y + i, rect.x + rect.w - 1, rect.y + i, c);
    }
}

void FillHorizontalGradient(Renderer &r, const UiRect &rect, SDL_Color left, SDL_Color right)
{
    if (rect.h <= 0 || rect.w <= 0)
        return;
    for (int i = 0; i < rect.w; ++i)
    {
        float t = (rect.w > 1) ? static_cast<float>(i) / static_cast<float>(rect.w - 1) : 0.0f;
        SDL_Color c = Mix(left, right, t);
        r.DrawLineScreen(rect.x + i, rect.y, rect.x + i, rect.y + rect.h - 1, c);
    }
}

namespace
{

CachedSprite sPanelFill {"dialog_items.panel_fill", SpriteKind::Frame};   // 160x14 metallic scanline panel fill
CachedSprite sTopRail {"dialog_items.top_rail", SpriteKind::Frame};       // 35x3 top horizontal bevel rail
CachedSprite sBottomRail {"dialog_items.bottom_rail", SpriteKind::Frame}; // 33x3 bottom horizontal bevel rail
CachedSprite sSideRail {"dialog_items.side_rail", SpriteKind::Frame};     // 4x41 vertical bevel rail (flip H for right)
CachedSprite sCornerCap {"dialog_items.corner_cap", SpriteKind::Frame};   // 4x4 panel corner cap
CachedSprite sTlRailV {"dialog_items.tl_rail_v", SpriteKind::Frame};      // 10x51 ornate top-left vertical rail
CachedSprite sTlRailH {"dialog_items.tl_rail_h", SpriteKind::Frame};      // 45x6 ornate top-left horizontal rail
CachedSprite sTlCorner {"dialog_items.tl_corner", SpriteKind::Frame};     // 11x11 ornate top-left corner bracket
CachedSprite sBrRailV {"dialog_items.br_rail_v", SpriteKind::Frame};      // 10x46 ornate bottom-right vertical rail
CachedSprite sBrRailH {"dialog_items.br_rail_h", SpriteKind::Frame};      // 45x6 ornate bottom-right horizontal rail
CachedSprite sBrCorner {"dialog_items.br_corner", SpriteKind::Frame};     // 11x11 ornate bottom-right corner bracket
CachedSprite sTabFillActive {"dialog_items.tab_fill_active", SpriteKind::Frame}; // 30x15 active tab interior fill
CachedSprite sTabFillIdle {"dialog_items.tab_fill_idle", SpriteKind::Frame};     // 30x16 inactive tab interior fill
CachedSprite sTabShoulderL {"dialog_items.tab_shoulder_l", SpriteKind::Frame};   // 22x9 angled top-left tab shoulder
CachedSprite sTabShoulderR {"dialog_items.tab_shoulder_r", SpriteKind::Frame};   // 21x9 angled top-right tab shoulder

bool ColorMatches(SDL_Color a, SDL_Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool PanelSpritesReady()
{
    return sPanelFill.Get() >= 0 && sTopRail.Get() >= 0 && sBottomRail.Get() >= 0 && sSideRail.Get() >= 0
           && sCornerCap.Get() >= 0;
}

bool OrnateCornerSpritesReady()
{
    return sTlRailV.Get() >= 0 && sTlRailH.Get() >= 0 && sTlCorner.Get() >= 0 && sBrRailV.Get() >= 0
           && sBrRailH.Get() >= 0 && sBrCorner.Get() >= 0;
}

bool TabSpritesReady()
{
    return sTabFillActive.Get() >= 0 && sTabFillIdle.Get() >= 0 && sTabShoulderL.Get() >= 0 && sTabShoulderR.Get() >= 0
           && sTopRail.Get() >= 0 && sSideRail.Get() >= 0;
}

void TileHorizSpan(
    Renderer &r,
    int spriteId,
    int x,
    int y,
    int spanW,
    int drawH,
    float trimL,
    float trimT,
    float trimR,
    float trimB,
    float tileW,
    SDL_FlipMode flip = SDL_FLIP_NONE,
    Uint8 alpha = 255)
{
    if (spriteId < 0 || spanW <= 0 || drawH <= 0 || tileW <= 0.0f)
        return;
    float curX = static_cast<float>(x);
    float rem = static_cast<float>(spanW);
    while (rem > 0.5f)
    {
        const float segW = std::min(tileW, rem);
        const float extraR = tileW - segW;
        r.DrawSpritePartScreen(
            spriteId,
            {curX, static_cast<float>(y), segW, static_cast<float>(drawH)},
            trimL,
            trimT,
            trimR + extraR,
            trimB,
            flip,
            Faction::Resistance,
            alpha);
        curX += segW;
        rem -= segW;
    }
}

void TileVertSpan(
    Renderer &r,
    int spriteId,
    int x,
    int y,
    int drawW,
    int spanH,
    float trimL,
    float trimT,
    float trimR,
    float trimB,
    float tileH,
    SDL_FlipMode flip = SDL_FLIP_NONE,
    Uint8 alpha = 255)
{
    if (spriteId < 0 || drawW <= 0 || spanH <= 0 || tileH <= 0.0f)
        return;
    float curY = static_cast<float>(y);
    float rem = static_cast<float>(spanH);
    while (rem > 0.5f)
    {
        const float segH = std::min(tileH, rem);
        const float extraB = tileH - segH;
        r.DrawSpritePartScreen(
            spriteId,
            {static_cast<float>(x), curY, static_cast<float>(drawW), segH},
            trimL,
            trimT,
            trimR,
            trimB + extraB,
            flip,
            Faction::Resistance,
            alpha);
        curY += segH;
        rem -= segH;
    }
}

void TilePanelInterior(Renderer &r, const UiRect &rect, Uint8 alpha)
{
    constexpr float kTileW = 148.0f; // 160 - (6 + 6)
    constexpr float kTileH = 12.0f;  // 14 - (1 + 1)
    float curY = static_cast<float>(rect.y);
    float remY = static_cast<float>(rect.h);
    while (remY > 0.5f)
    {
        const float segH = std::min(kTileH, remY);
        const float extraB = kTileH - segH;
        TileHorizSpan(
            r,
            sPanelFill.Get(),
            rect.x,
            static_cast<int>(curY),
            rect.w,
            static_cast<int>(segH),
            6.0f,
            1.0f,
            6.0f,
            1.0f + extraB,
            kTileW,
            SDL_FLIP_NONE,
            alpha);
        curY += segH;
        remY -= segH;
    }
}

} // namespace

void DrawPanelFrame(Renderer &r, const UiRect &rect, SDL_Color border)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;

    if (!PanelSpritesReady() || rect.w < 12 || rect.h < 12)
    {
        r.DrawLineScreen(rect.x + 1, rect.y + 1, rect.Right() - 2, rect.y + 1, {255, 255, 255, 14});
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, border);
        return;
    }

    const Uint8 frameAlpha = border.a > 0 ? border.a : 255;

    // 4px side rails and horizontal 3px rails inset from 4x4 corner caps
    TileHorizSpan(
        r, sTopRail.Get(), rect.x + 4, rect.y, rect.w - 8, 3, 4.0f, 0.0f, 4.0f, 0.0f, 27.0f, SDL_FLIP_NONE, frameAlpha);
    TileHorizSpan(
        r,
        sBottomRail.Get(),
        rect.x + 4,
        rect.Bottom() - 3,
        rect.w - 8,
        3,
        4.0f,
        0.0f,
        4.0f,
        0.0f,
        25.0f,
        SDL_FLIP_NONE,
        frameAlpha);
    TileVertSpan(
        r,
        sSideRail.Get(),
        rect.x,
        rect.y + 4,
        4,
        rect.h - 8,
        0.0f,
        3.0f,
        0.0f,
        3.0f,
        35.0f,
        SDL_FLIP_NONE,
        frameAlpha);
    TileVertSpan(
        r,
        sSideRail.Get(),
        rect.Right() - 4,
        rect.y + 4,
        4,
        rect.h - 8,
        0.0f,
        3.0f,
        0.0f,
        3.0f,
        35.0f,
        SDL_FLIP_HORIZONTAL,
        frameAlpha);

    // 4x4 corner caps on all four corners (keep top-right and bottom-left crisp on large panels too)
    r.DrawSpritePartScreen(
        sCornerCap.Get(),
        {static_cast<float>(rect.x), static_cast<float>(rect.y), 4.0f, 4.0f},
        0,
        0,
        0,
        0,
        SDL_FLIP_NONE,
        Faction::Resistance,
        frameAlpha);
    r.DrawSpritePartScreen(
        sCornerCap.Get(),
        {static_cast<float>(rect.Right() - 4), static_cast<float>(rect.y), 4.0f, 4.0f},
        0,
        0,
        0,
        0,
        SDL_FLIP_HORIZONTAL,
        Faction::Resistance,
        frameAlpha);
    r.DrawSpritePartScreen(
        sCornerCap.Get(),
        {static_cast<float>(rect.x), static_cast<float>(rect.Bottom() - 4), 4.0f, 4.0f},
        0,
        0,
        0,
        0,
        SDL_FLIP_VERTICAL,
        Faction::Resistance,
        frameAlpha);
    r.DrawSpritePartScreen(
        sCornerCap.Get(),
        {static_cast<float>(rect.Right() - 4), static_cast<float>(rect.Bottom() - 4), 4.0f, 4.0f},
        0,
        0,
        0,
        0,
        static_cast<SDL_FlipMode>(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL),
        Faction::Resistance,
        frameAlpha);

    // Ornate stepped brackets & extended corner rails for larger panels
    if (OrnateCornerSpritesReady() && rect.w >= 140 && rect.h >= 120)
    {
        r.DrawSpritePartScreen(
            sTlRailV.Get(),
            {static_cast<float>(rect.x), static_cast<float>(rect.y + 11), 10.0f, 51.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);
        r.DrawSpritePartScreen(
            sTlRailH.Get(),
            {static_cast<float>(rect.x + 11), static_cast<float>(rect.y), 45.0f, 6.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);
        r.DrawSpritePartScreen(
            sTlCorner.Get(),
            {static_cast<float>(rect.x), static_cast<float>(rect.y), 11.0f, 11.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);

        r.DrawSpritePartScreen(
            sBrRailV.Get(),
            {static_cast<float>(rect.Right() - 10), static_cast<float>(rect.Bottom() - 57), 10.0f, 46.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);
        r.DrawSpritePartScreen(
            sBrRailH.Get(),
            {static_cast<float>(rect.Right() - 56), static_cast<float>(rect.Bottom() - 6), 45.0f, 6.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);
        r.DrawSpritePartScreen(
            sBrCorner.Get(),
            {static_cast<float>(rect.Right() - 11), static_cast<float>(rect.Bottom() - 11), 11.0f, 11.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            frameAlpha);
    }

    // Preserve semantic highlight/warning/error border tinting (e.g. Accent/Danger) when non-neutral
    if (!ColorMatches(border, color::Border) && !ColorMatches(border, color::BorderStrong) && border.a > 0)
        r.DrawRectScreen(
            rect.x, rect.y, rect.w, rect.h, WithAlpha(border, static_cast<Uint8>(std::min<int>(border.a, 180))));
}

void DrawPanel(Renderer &r, const UiRect &rect, SDL_Color fill, SDL_Color border)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;

    r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
    if (PanelSpritesReady() && rect.w >= 12 && rect.h >= 12)
    {
        const UiRect inner {rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4};
        TilePanelInterior(r, inner, fill.a);
    }
    DrawPanelFrame(r, rect, border);
}

void DrawTab(
    Renderer &r,
    const UiRect &rect,
    bool active,
    bool hovered,
    SDL_Color accent,
    BitmapFontRenderer *font,
    std::string_view label)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;

    if (!TabSpritesReady() || rect.w < 44 || rect.h < 14)
    {
        r.FillRectScreen(
            rect.x,
            rect.y,
            rect.w,
            rect.h,
            active ? color::Hover : (hovered ? color::PanelRaised : color::PanelSunken));
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, active ? color::BorderStrong : color::Border);
        if (active)
            r.FillRectScreen(rect.x, rect.Bottom() - 3, rect.w, 3, accent);
    }
    else
    {
        const SDL_Color baseFill = active ? color::Hover : (hovered ? color::PanelRaised : color::PanelSunken);
        const Uint8 tabAlpha = active ? 245 : (hovered ? 225 : 195);

        // Angled trapezoid backdrop matching sTabShoulderL (22x9) and sTabShoulderR (21x9)
        const int shoulderH = std::min(9, rect.h);
        for (int row = 0; row < shoulderH; ++row)
        {
            const int insetL = (22 * (shoulderH - 1 - row)) / shoulderH;
            const int insetR = (21 * (shoulderH - 1 - row)) / shoulderH;
            const int lineX = rect.x + insetL;
            const int lineEnd = rect.Right() - 1 - insetR;
            if (lineX <= lineEnd)
                r.DrawLineScreen(lineX, rect.y + row, lineEnd, rect.y + row, baseFill);
        }
        if (rect.h > shoulderH)
            r.FillRectScreen(rect.x + 1, rect.y + shoulderH, rect.w - 2, rect.h - shoulderH, baseFill);

        // Interior sprite fill below top rail / between angled shoulders
        const int fillSprite = active ? sTabFillActive.Get() : sTabFillIdle.Get();
        const int fillTop = rect.y + 3;
        const int fillH = std::max(1, rect.h - 4);
        if (rect.w > 8)
        {
            r.DrawSpritePartScreen(
                fillSprite,
                {static_cast<float>(rect.x + 4),
                 static_cast<float>(fillTop),
                 static_cast<float>(rect.w - 8),
                 static_cast<float>(fillH)},
                2.0f,
                1.0f,
                2.0f,
                1.0f,
                SDL_FLIP_NONE,
                Faction::Resistance,
                tabAlpha);
        }

        // Top rail across the flat crown between the two angled shoulders
        const int crownX = rect.x + 22;
        const int crownW = rect.w - 43;
        if (crownW > 0)
            TileHorizSpan(
                r, sTopRail.Get(), crownX, rect.y, crownW, 3, 4.0f, 0.0f, 4.0f, 0.0f, 27.0f, SDL_FLIP_NONE, tabAlpha);

        // Side vertical rails below the 9px shoulders
        if (rect.h > 9)
        {
            TileVertSpan(
                r,
                sSideRail.Get(),
                rect.x,
                rect.y + 9,
                4,
                rect.h - 9,
                0.0f,
                3.0f,
                0.0f,
                3.0f,
                35.0f,
                SDL_FLIP_NONE,
                tabAlpha);
            TileVertSpan(
                r,
                sSideRail.Get(),
                rect.Right() - 4,
                rect.y + 9,
                4,
                rect.h - 9,
                0.0f,
                3.0f,
                0.0f,
                3.0f,
                35.0f,
                SDL_FLIP_HORIZONTAL,
                tabAlpha);
        }

        // Bottom rail for inactive tabs; active accent indicator on active tab
        if (active)
        {
            r.FillRectScreen(rect.x + 3, rect.Bottom() - 3, rect.w - 6, 3, accent);
        }
        else
        {
            TileHorizSpan(
                r,
                sBottomRail.Get(),
                rect.x + 4,
                rect.Bottom() - 3,
                rect.w - 8,
                3,
                4.0f,
                0.0f,
                4.0f,
                0.0f,
                25.0f,
                SDL_FLIP_NONE,
                tabAlpha);
        }

        // Left and right angled shoulder edges on top
        r.DrawSpritePartScreen(
            sTabShoulderL.Get(),
            {static_cast<float>(rect.x), static_cast<float>(rect.y), 22.0f, 9.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            tabAlpha);
        r.DrawSpritePartScreen(
            sTabShoulderR.Get(),
            {static_cast<float>(rect.Right() - 21), static_cast<float>(rect.y), 21.0f, 9.0f},
            0,
            0,
            0,
            0,
            SDL_FLIP_NONE,
            Faction::Resistance,
            tabAlpha);
    }

    if (font && !label.empty())
    {
        const SDL_Color textColor = active ? color::Text : (hovered ? color::Text : color::TextMuted);
        const std::string drawn = FitText(*font, label, FONT_BODY_BOLD, std::max(0, rect.w - 24));
        font->DrawString(
            drawn, rect.CenterX(), rect.CenterY(), FONT_BODY_BOLD, textColor, true, Anchor::HCENTER | Anchor::VCENTER);
    }
}

UiRect DrawDialog(
    Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const UiRect &rect, std::string_view title)
{
    r.FillRectScreen(0, 0, windowW, windowH, color::Scrim);

    // Layered drop shadow for clear separation above the world/backdrop
    r.FillRectScreen(rect.x + 8, rect.y + 10, rect.w, rect.h, {0, 0, 0, 62});
    r.FillRectScreen(rect.x + 4, rect.y + 5, rect.w, rect.h, {0, 0, 0, 108});
    DrawPanel(r, rect, color::Panel, color::BorderStrong);

    const int titleH = 52;
    const int innerW = std::max(0, rect.w - 8);
    FillVerticalGradient(
        r,
        {rect.x + 4, rect.y + 4, innerW, titleH - 4},
        WithAlpha(Mix(color::PanelRaised, color::AccentDeep, 0.34f), 205),
        WithAlpha(color::PanelSunken, 130));
    if (rect.w > 28)
        r.FillRectScreen(rect.x + 14, rect.y + 5, rect.w - 28, 1, WithAlpha(color::Accent, 72));
    // Tactical left marker pill beside title text
    r.FillRectScreen(rect.x + 16, rect.y + 15, 4, 22, color::Accent);
    font.DrawString(
        FitText(font, title, FONT_H2, std::max(40, rect.w - 48)),
        rect.x + 28,
        rect.y + titleH / 2,
        FONT_H2,
        color::Text,
        true,
        Anchor::LEFT | Anchor::VCENTER);

    r.FillRectScreen(rect.x + 4, rect.y + titleH, innerW, 1, color::Border);
    r.FillRectScreen(rect.x + 16, rect.y + titleH - 1, std::min(112, std::max(0, rect.w - 32)), 2, color::Accent);

    // Subtle recessed perimeter framing the content compartment in the padding gutter
    if (rect.w > 32 && rect.h > titleH + 40)
    {
        const UiRect well {rect.x + 12, rect.y + titleH + 8, rect.w - 24, rect.h - titleH - 20};
        r.DrawRectScreen(well.x, well.y, well.w, well.h, WithAlpha(color::Border, 85));
        r.DrawLineScreen(well.x + 1, well.y + 1, well.Right() - 2, well.y + 1, {255, 255, 255, 14});
    }

    return {rect.x + 20, rect.y + titleH + 16, rect.w - 40, rect.h - titleH - 32};
}

int ButtonFontForScale(float scale)
{
    struct Option
    {
        int id;
        float px;
    };
    static const Option kBold[]
        = {{FONT_SMALL_BOLD, 13.0f}, {FONT_BODY_BOLD, 16.0f}, {FONT_H2, 22.0f}, {FONT_H1, 30.0f}, {FONT_HERO, 48.0f}};
    const float want = 16.0f * (scale > 0.0f ? scale : 1.0f);
    int best = FONT_BODY_BOLD;
    float bestDiff = 1e9f;
    for (const Option &o : kBold)
    {
        const float d = std::abs(o.px - want);
        if (d < bestDiff)
        {
            bestDiff = d;
            best = o.id;
        }
    }
    return best;
}

namespace
{

SDL_Color ScaleAlpha(SDL_Color c, Uint8 opacity)
{
    c.a = static_cast<Uint8>(static_cast<unsigned>(c.a) * opacity / 255);
    return c;
}

void DrawCornerBrackets(Renderer &r, const UiRect &rect, SDL_Color col, int arm)
{
    if (col.a == 0 || arm < 2 || rect.w < arm * 2 + 2 || rect.h < arm * 2 + 2)
        return;
    const int x0 = rect.x;
    const int y0 = rect.y;
    const int x1 = rect.Right() - arm;
    const int y1 = rect.Bottom() - arm;
    const int rx = rect.Right() - 1;
    const int by = rect.Bottom() - 1;

    // Four crisp L-brackets framing interactive controls
    r.FillRectScreen(x0, y0, arm, 1, col);
    r.FillRectScreen(x0, y0, 1, arm, col);
    r.FillRectScreen(x1, y0, arm, 1, col);
    r.FillRectScreen(rx, y0, 1, arm, col);
    r.FillRectScreen(x0, by, arm, 1, col);
    r.FillRectScreen(x0, y1, 1, arm, col);
    r.FillRectScreen(x1, by, arm, 1, col);
    r.FillRectScreen(rx, y1, 1, arm, col);
}

} // namespace

void DrawButtonChrome(
    Renderer &r,
    const UiRect &rect,
    SDL_Color topFill,
    SDL_Color bottomFill,
    SDL_Color border,
    SDL_Color accentBar,
    SDL_Color cornerColor,
    Uint8 opacity,
    bool horizontalGradient)
{
    if (rect.w <= 0 || rect.h <= 0 || opacity == 0)
        return;

    const SDL_Color topCol = ScaleAlpha(topFill, opacity);
    const SDL_Color botCol = ScaleAlpha(bottomFill, opacity);
    const SDL_Color rimCol = ScaleAlpha(border, opacity);
    const SDL_Color barCol = ScaleAlpha(accentBar, opacity);
    const SDL_Color brkCol = ScaleAlpha(cornerColor, opacity);

    if (horizontalGradient)
        FillHorizontalGradient(r, rect, topCol, botCol);
    else
        FillVerticalGradient(r, rect, topCol, botCol);

    if (!horizontalGradient && PanelSpritesReady() && rect.w >= 12 && rect.h >= 12)
        TilePanelInterior(r, rect, static_cast<Uint8>(44 * opacity / 255));

    if (!horizontalGradient && rect.w >= 6 && rect.h >= 6)
    {
        SDL_Color specular = Mix(topCol, color::White, 0.32f);
        specular.a = static_cast<Uint8>(static_cast<unsigned>(topCol.a) * 115 / 255);
        r.FillRectScreen(rect.x + 1, rect.y + 1, rect.w - 2, 1, specular);

        SDL_Color shadow {4, 6, 10, static_cast<Uint8>(static_cast<unsigned>(botCol.a) * 140 / 255)};
        r.FillRectScreen(rect.x + 1, rect.Bottom() - 2, rect.w - 2, 1, shadow);
    }

    if (rimCol.a > 0 && !horizontalGradient)
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, rimCol);

    if (barCol.a > 0 && rect.w >= 12 && rect.h >= 6)
    {
        const int barW = horizontalGradient ? 4 : 3;
        r.FillRectScreen(rect.x + 1, rect.y + 1, barW, rect.h - 2, barCol);
        SDL_Color barCore = Mix(barCol, color::White, 0.35f);
        barCore.a = static_cast<Uint8>(static_cast<unsigned>(barCol.a) * 180 / 255);
        r.FillRectScreen(rect.x + 1, rect.y + 3, 1, std::max(1, rect.h - 6), barCore);
    }

    const int arm = std::clamp(std::min(rect.w, rect.h) / 5, 3, 7);
    if (!horizontalGradient)
        DrawCornerBrackets(r, rect, brkCol, arm);
}

void DrawButton(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view label,
    ButtonStyle style,
    bool hovered,
    bool enabled,
    int fontId,
    Uint8 opacity)
{
    if (hovered && enabled)
        SoundRequests::MarkHover();

    SDL_Color topFill = color::PanelRaised;
    SDL_Color botFill = color::PanelSunken;
    SDL_Color border = color::BorderStrong;
    constexpr SDL_Color accentBar {0, 0, 0, 0};
    SDL_Color corners {0, 0, 0, 0};
    SDL_Color text = color::Text;

    switch (style)
    {
        case ButtonStyle::Primary:
            topFill = Mix(color::Accent, color::White, hovered ? 0.36f : 0.16f);
            botFill = hovered ? color::Accent : Mix(color::AccentDeep, color::Accent, 0.38f);
            border = hovered ? SDL_Color {255, 244, 210, 255} : SDL_Color {255, 224, 154, 255};
            corners = hovered ? color::White : SDL_Color {255, 236, 184, 255};
            text = color::AccentText;
            break;
        case ButtonStyle::Secondary:
            topFill
                = hovered ? Mix(color::Hover, color::Accent, 0.22f) : Mix(color::PanelRaised, color::AccentDeep, 0.22f);
            botFill = hovered ? Mix(color::PanelRaised, color::AccentDeep, 0.28f)
                              : Mix(color::Panel, color::PanelSunken, 0.45f);
            border = hovered ? Mix(color::Accent, color::White, 0.20f) : Mix(color::BorderStrong, color::Accent, 0.30f);
            corners
                = hovered ? Mix(color::Accent, color::White, 0.34f) : Mix(color::Accent, color::BorderStrong, 0.50f);
            text = hovered ? color::White : color::Text;
            break;
        case ButtonStyle::Danger:
            topFill = hovered ? SDL_Color {148, 46, 38, 255} : SDL_Color {104, 34, 28, 255};
            botFill = hovered ? SDL_Color {80, 24, 20, 255} : color::DangerDeep;
            border = hovered ? Mix(color::Danger, color::White, 0.25f) : SDL_Color {158, 58, 48, 255};
            corners = hovered ? SDL_Color {255, 204, 196, 255} : Mix(color::Danger, color::White, 0.15f);
            text = hovered ? color::White : SDL_Color {255, 220, 212, 255};
            break;
        case ButtonStyle::Ghost:
            topFill = hovered ? Mix(color::Hover, color::AccentDeep, 0.36f) : WithAlpha(color::PanelRaised, 95);
            botFill = hovered ? WithAlpha(color::Panel, 225) : WithAlpha(color::PanelSunken, 115);
            border = hovered ? color::BorderStrong : WithAlpha(color::Border, 130);
            corners = hovered ? color::Accent : WithAlpha(color::BorderStrong, 150);
            text = hovered ? color::Text : color::TextDim;
            break;
    }

    if (!enabled)
    {
        topFill = Mix(color::PanelRaised, color::AccentDeep, 0.10f);
        botFill = color::PanelSunken;
        border = Mix(color::Border, color::AccentDeep, 0.28f);
        corners = WithAlpha(color::Accent, 76);
        text = Mix(color::TextDim, color::TextMuted, 0.45f);
    }

    DrawButtonChrome(r, rect, topFill, botFill, border, accentBar, corners, opacity, false);
    text = ScaleAlpha(text, opacity);
    std::string shown = FitText(font, label, fontId, rect.w - 16);
    font.DrawString(
        shown,
        rect.CenterX(),
        rect.CenterY(),
        fontId,
        text,
        style != ButtonStyle::Primary,
        Anchor::HCENTER | Anchor::VCENTER);
}

void DrawCheckbox(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view label,
    bool checked,
    bool hovered,
    bool enabled)
{
    if (hovered && enabled)
        SoundRequests::MarkHover();

    const int boxSize = std::clamp(rect.h - 6, 22, 26);
    const UiRect box = {rect.x, rect.CenterY() - boxSize / 2, boxSize, boxSize};

    const SDL_Color topFill = !enabled               ? color::PanelSunken
                              : (checked || hovered) ? Mix(color::Hover, color::AccentDeep, 0.34f)
                                                     : Mix(color::PanelRaised, color::PanelSunken, 0.45f);
    const SDL_Color botFill = !enabled ? color::PanelSunken : color::PanelSunken;
    const SDL_Color border = !enabled  ? color::Border
                             : hovered ? color::Accent
                             : checked ? Mix(color::BorderStrong, color::Accent, 0.45f)
                                       : Mix(color::BorderStrong, color::AccentDeep, 0.30f);

    FillVerticalGradient(r, box, topFill, botFill);
    r.DrawRectScreen(box.x, box.y, box.w, box.h, border);
    if (enabled)
    {
        r.FillRectScreen(box.x + 1, box.y + 1, std::max(0, box.w - 2), 1, WithAlpha(color::White, 22));
    }

    if (checked)
    {
        const SDL_Color checkCol = enabled ? color::Accent : color::TextMuted;
        const int bx = box.x + (boxSize - 20) / 2;
        const int by = box.y + (boxSize - 20) / 2;
        for (int i = 0; i < 4; ++i)
        {
            r.FillRectScreen(bx + 4 + i, by + 10 + i, 2, 2, checkCol);
        }
        for (int i = 0; i < 8; ++i)
        {
            r.FillRectScreen(bx + 7 + i, by + 13 - i, 2, 2, checkCol);
        }
    }

    if (!label.empty())
    {
        const SDL_Color textCol = !enabled               ? color::TextMuted
                                  : (checked || hovered) ? color::Text
                                                         : Mix(color::Text, color::TextDim, 0.40f);
        font.DrawString(
            label, box.Right() + 10, rect.CenterY(), FONT_BODY_BOLD, textCol, false, Anchor::LEFT | Anchor::VCENTER);
    }
}

void DrawProgressBar(Renderer &r, const UiRect &rect, float ratio, SDL_Color fillColor)
{
    r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, color::PanelSunken);
    int fw = static_cast<int>((rect.w - 2) * std::clamp(ratio, 0.0f, 1.0f));
    if (fw > 0)
        r.FillRectScreen(rect.x + 1, rect.y + 1, fw, rect.h - 2, fillColor);
    r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, color::Border);
}

UiRect ScrollThumb(const UiRect &track, float pos, float total, float visible)
{
    const bool vertical = track.h >= track.w;
    const int len = vertical ? track.h : track.w;
    if (total <= visible || len <= 0)
        return {track.x, track.y, vertical ? track.w : len, vertical ? len : track.h};
    const int thumb = std::max(20, static_cast<int>(len * visible / total));
    const int at = static_cast<int>((len - thumb) * std::clamp(pos / (total - visible), 0.0f, 1.0f));
    return vertical ? UiRect {track.x, track.y + at, track.w, thumb} : UiRect {track.x + at, track.y, thumb, track.h};
}

void DrawScrollBar(Renderer &r, const UiRect &track, float pos, float total, float visible, bool hot)
{
    if (total <= visible)
        return;
    r.FillRectScreen(track.x, track.y, track.w, track.h, WithAlpha(color::PanelSunken, 220));
    r.DrawRectScreen(track.x, track.y, track.w, track.h, WithAlpha(color::Border, 140));
    const UiRect t = ScrollThumb(track, pos, total, visible);
    const SDL_Color top = hot ? color::Accent : Mix(color::BorderStrong, color::Accent, 0.35f);
    const SDL_Color bot
        = hot ? Mix(color::AccentDeep, color::Accent, 0.45f) : Mix(color::PanelRaised, color::Border, 0.45f);
    FillVerticalGradient(r, t, top, bot);
    r.DrawRectScreen(t.x, t.y, t.w, t.h, hot ? Mix(color::Accent, color::White, 0.25f) : color::BorderStrong);
}

float ScrollPosFromMouse(const UiRect &track, int mouse, float total, float visible)
{
    if (total <= visible)
        return 0.0f;
    const bool vertical = track.h >= track.w;
    const int len = vertical ? track.h : track.w;
    const int thumb = std::max(20, static_cast<int>(len * visible / total));
    const int start = vertical ? track.y : track.x;
    const float ratio
        = std::clamp(static_cast<float>(mouse - start - thumb / 2) / std::max(1, len - thumb), 0.0f, 1.0f);
    return ratio * (total - visible);
}

bool OnScrollTrack(const UiRect &track, int mx, int my)
{
    const bool vertical = track.h >= track.w;
    const int padX = vertical ? 8 : 0, padY = vertical ? 0 : 8;
    return track.w > 0 && track.h > 0 && mx >= track.x - padX && mx <= track.Right() + padX && my >= track.y - padY
           && my <= track.Bottom() + padY;
}

namespace
{
// Shared kinetic vertical drag + flick glide + smooth wheel easing solver
void StepVerticalKineticScroll(
    float &offset,
    float &target,
    float &velocity,
    bool &dragging,
    bool &dragMoved,
    int &dragStartY,
    int &dragLastY,
    Uint32 &lastTickMs,
    const UiRect &area,
    float maxScroll,
    int stepPx,
    int mx,
    int my,
    bool down,
    bool pressed,
    int pressX,
    int pressY,
    int wheel,
    bool allowWheelOutside = false,
    const UiRect *extraWheelRect = nullptr)
{
    constexpr float kSlop = 12.0f;
    constexpr float kGlideMax = 6000.0f;
    constexpr float kGlideStop = 30.0f;
    constexpr float kGlideFriction = 3.5f;
    constexpr float kWheelEase = 18.0f;

    const Uint32 nowMs = SDL_GetTicks();
    const float dtS = lastTickMs == 0 ? 0.0f : std::clamp(static_cast<float>(nowMs - lastTickMs) * 0.001f, 0.0f, 0.1f);
    lastTickMs = nowMs;

    if (!dragging && wheel != 0 && maxScroll > 0.0f
        && (allowWheelOutside || area.Contains(mx, my) || (extraWheelRect && extraWheelRect->Contains(mx, my))))
    {
        velocity = 0.0f;
        target = std::clamp(target - static_cast<float>(wheel * stepPx), 0.0f, maxScroll);
    }

    if (pressed && down && maxScroll > 0.0f && area.Contains(pressX, pressY))
    {
        velocity = 0.0f;
        dragging = true;
        dragMoved = false;
        dragStartY = pressY;
        dragLastY = pressY;
    }

    if (dragging)
    {
        bool justMoved = false;
        if (!dragMoved && std::abs(my - dragStartY) > kSlop)
        {
            dragMoved = true;
            justMoved = true;
        }
        if (dragMoved)
        {
            const float dy = static_cast<float>(my - dragLastY);
            offset -= dy;
            target = offset;
            dragLastY = my;
            if (dtS > 0.0f && !justMoved)
            {
                const float k = std::min(1.0f, dtS / 0.08f);
                velocity += (std::clamp(-dy / dtS, -kGlideMax, kGlideMax) - velocity) * k;
            }
        }
        if (!down)
            dragging = false;
    }
    else if (std::abs(velocity) >= kGlideStop && maxScroll > 0.0f)
    {
        offset += velocity * dtS;
        target = offset;
        velocity *= std::exp(-dtS * kGlideFriction);
    }
    else
    {
        velocity = 0.0f;
        if (dtS > 0.0f && std::abs(target - offset) > 0.35f)
            offset += (target - offset) * (1.0f - std::exp(-dtS * kWheelEase));
        else
            offset = target;
    }

    const float clamped = std::clamp(offset, 0.0f, maxScroll);
    if (clamped != offset)
    {
        offset = clamped;
        velocity = 0.0f;
    }
    target = std::clamp(target, 0.0f, maxScroll);
}
} // namespace

void SmoothListScroll::EnsureVisible(int row, int stepH, int viewH, int totalRows)
{
    if (stepH <= 0 || totalRows <= 0)
    {
        Reset();
        return;
    }
    const float maxScroll = std::max(0.0f, static_cast<float>(totalRows * stepH - viewH));
    const float topPx = static_cast<float>(row * stepH);
    const float botPx = static_cast<float>((row + 1) * stepH);
    if (topPx < target)
        target = topPx;
    else if (botPx > target + static_cast<float>(viewH))
        target = botPx - static_cast<float>(viewH);
    target = std::clamp(target, 0.0f, maxScroll);
    offset = std::clamp(offset, 0.0f, maxScroll);
}

void UpdateSmoothList(
    SmoothListScroll &scroll,
    const UiRect &listRect,
    const UiRect &trackRect,
    int contentH,
    int stepPx,
    int mx,
    int my,
    bool down,
    bool pressed,
    int pressX,
    int pressY,
    int wheel,
    bool &clicked,
    bool allowBarDrag)
{
    const float maxScroll = std::max(0.0f, static_cast<float>(contentH - listRect.h));
    if (maxScroll <= 0.0f)
    {
        scroll.Reset();
        return;
    }
    const bool touchProfile = uilayout::MenuProfile(uilayout::Current(), "main_menu") == "touch";
    if (allowBarDrag && !touchProfile && pressed && down && OnScrollTrack(trackRect, pressX, pressY))
    {
        scroll.barDrag = true;
        scroll.velocity = 0.0f;
        scroll.dragging = false;
        scroll.dragMoved = false;
    }
    if (scroll.barDrag)
    {
        const float pos
            = ScrollPosFromMouse(trackRect, my, static_cast<float>(contentH), static_cast<float>(listRect.h));
        scroll.offset = scroll.target = std::clamp(pos, 0.0f, maxScroll);
        if (!down)
        {
            scroll.barDrag = false;
            clicked = false;
        }
        return;
    }

    const bool wasDragging = scroll.dragging;
    StepVerticalKineticScroll(
        scroll.offset,
        scroll.target,
        scroll.velocity,
        scroll.dragging,
        scroll.dragMoved,
        scroll.dragStartY,
        scroll.dragLastY,
        scroll.lastTickMs,
        listRect,
        maxScroll,
        stepPx,
        mx,
        my,
        down,
        pressed,
        pressX,
        pressY,
        wheel);
    if (wasDragging && !scroll.dragging && scroll.dragMoved)
    {
        scroll.dragMoved = false;
        clicked = false;
    }
}

namespace
{
// ui/layout.yaml (screen confirm: a bigger dialog with big buttons with the touch interface); the numbers stand in
// without it
struct ConfirmRects
{
    UiRect dialog, yes, no;
};
ConfirmRects ConfirmLayout(int windowW, int windowH)
{
    ConfirmRects r;
    const uilayout::Layout &file = uilayout::Current();
    if (file.Has("confirm", "pc"))
    {
        const uilayout::Solved s = file.Solve("confirm", uilayout::MenuProfile(file, "confirm"), windowW, windowH, {});
        r.dialog = s.Rect("dialog");
        r.yes = s.Rect("yes");
        r.no = s.Rect("no");
        return r;
    }
    r.dialog = CenteredRect(windowW, windowH, 520, 236);
    r.yes = {r.dialog.x + 24, r.dialog.Bottom() - 70, (r.dialog.w - 60) / 2, 48};
    r.no = {r.dialog.Right() - 24 - (r.dialog.w - 60) / 2, r.dialog.Bottom() - 70, (r.dialog.w - 60) / 2, 48};
    return r;
}
} // namespace

// The touch interface draws and hit-tests the question itself (big buttons from the layout file); ImGui's modal is
// the desktop's
static bool ConfirmIsNative()
{
    const uilayout::Layout &file = uilayout::Current();
    return !ImGuiBridge::IsInitialized() || uilayout::MenuProfile(file, "confirm") == "touch";
}

int ConfirmUpdate(
    const ConfirmState &state,
    int windowW,
    int windowH,
    int mx,
    int my,
    bool clicked,
    bool enterPressed,
    bool escPressed)
{
    if (!state.open)
    {
        state.modalResult = -1;
        return -1;
    }
    if (state.modalResult >= 0)
    {
        const int ans = state.modalResult;
        state.modalResult = -1;
        return ans;
    }
    const bool useNativeHitTest = ConfirmIsNative();
    const ConfirmRects c = ConfirmLayout(windowW, windowH);
    if (enterPressed || (useNativeHitTest && clicked && c.yes.Contains(mx, my)))
    {
        state.modalResult = -1;
        return 1;
    }
    if (escPressed || (useNativeHitTest && clicked && c.no.Contains(mx, my)))
    {
        state.modalResult = -1;
        return 0;
    }
    return -1;
}

void DrawConfirm(
    Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const ConfirmState &state, int mx, int my)
{
    if (!state.open)
        return;
    if (!ConfirmIsNative() && ImGuiBridge::IsFrameActive())
    {
        ImGuiBridge::DrawConfirmModal(const_cast<ConfirmState &>(state), windowW, windowH);
        return;
    }
    const ConfirmRects c = ConfirmLayout(windowW, windowH);
    const UiRect content = DrawDialog(r, font, windowW, windowH, c.dialog, state.title);
    const int wellH = std::max(44, c.yes.y - content.y - 16);
    const UiRect well {content.x + 8, content.y + 2, std::max(40, content.w - 16), wellH};
    DrawPanel(r, well, WithAlpha(color::PanelSunken, 220), WithAlpha(color::Border, 165));
    r.FillRectScreen(well.x + 2, well.y + 6, 3, std::max(8, well.h - 12), color::Danger);
    font.DrawTextWrapped(
        state.text, well.x + 18, well.y + 14, std::max(40, well.w - 32), FONT_BODY, color::Text, false);
    DrawButton(r, font, c.yes, _("YES"), ButtonStyle::Danger, c.yes.Contains(mx, my));
    DrawButton(r, font, c.no, _("NO"), ButtonStyle::Secondary, c.no.Contains(mx, my));
}

void DrawComboField(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view value,
    bool open,
    bool hovered,
    bool enabled,
    SDL_Color valueColor,
    int fontId)
{
    if (hovered && enabled)
        SoundRequests::MarkHover();
    const bool hot = enabled && (open || hovered);
    const SDL_Color topFill = !enabled  ? color::PanelSunken
                              : open    ? Mix(color::Hover, color::AccentDeep, 0.36f)
                              : hovered ? Mix(color::Hover, color::AccentDeep, 0.24f)
                                        : Mix(color::PanelRaised, color::AccentDeep, 0.18f);
    const SDL_Color botFill = !enabled ? color::PanelSunken
                              : hot    ? Mix(color::PanelRaised, color::PanelSunken, 0.35f)
                                       : Mix(color::Panel, color::PanelSunken, 0.50f);
    const SDL_Color border = !enabled  ? Mix(color::Border, color::AccentDeep, 0.20f)
                             : open    ? color::Accent
                             : hovered ? Mix(color::Accent, color::BorderStrong, 0.55f)
                                       : Mix(color::BorderStrong, color::Accent, 0.28f);
    const SDL_Color corners = !enabled  ? SDL_Color {0, 0, 0, 0}
                              : open    ? color::Accent
                              : hovered ? Mix(color::Accent, color::BorderStrong, 0.45f)
                                        : Mix(color::AccentDeep, color::BorderStrong, 0.45f);
    const SDL_Color accentBar = open && enabled ? color::Accent : SDL_Color {0, 0, 0, 0};

    DrawButtonChrome(r, rect, topFill, botFill, border, accentBar, corners);

    const bool narrow = rect.w < 92; // compact table cell (Team / Colour)
    const int chosenFont = fontId == FONT_BODY && rect.w < 84 ? FONT_SMALL : fontId;
    const int arrowZoneW = narrow ? 20 : 28;
    const int padL = narrow ? 6 : 12;
    const int divX = rect.Right() - arrowZoneW;
    if (divX > rect.x + padL + 8)
    {
        r.FillRectScreen(divX, rect.y + 4, 1, std::max(0, rect.h - 8), WithAlpha(border, 120));
    }
    font.DrawString(
        FitText(font, value, chosenFont, std::max(0, divX - (rect.x + padL) - 4)),
        rect.x + padL,
        rect.CenterY(),
        chosenFont,
        enabled ? valueColor : color::TextMuted,
        false,
        Anchor::LEFT | Anchor::VCENTER);

    // The arrow: a crisp triangle centered in its compartment, pointing up while the list is open
    const SDL_Color arrow
        = enabled ? (open || hovered ? color::Accent : Mix(color::TextDim, color::Accent, 0.25f)) : color::TextMuted;
    const int ax = rect.Right() - arrowZoneW / 2, ay = rect.CenterY();
    for (int i = 0; i < 5; ++i)
    {
        const int half = open ? i : 4 - i;
        r.FillRectScreen(ax - half, ay - 3 + i, half * 2 + 1, 1, arrow);
    }
}

UiRect ComboListRect(const UiRect &field, int itemCount, int windowH)
{
    const int visible = std::clamp(itemCount, 1, kComboMaxVisible);
    const int h = visible * kComboItemH + 6;
    int y = field.Bottom() + 2;
    if (y + h > windowH - 8)
        y = std::max(8, field.y - 2 - h);
    return {field.x, y, std::max(field.w, kComboMinListW), h};
}

void DrawComboList(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &field,
    const std::vector<std::string> &items,
    int current,
    const ComboState &state,
    int windowH,
    int mouseX,
    int mouseY,
    const std::vector<bool> *disabled,
    const std::vector<SDL_Color> *swatches)
{
    const int n = static_cast<int>(items.size());
    const UiRect list = ComboListRect(field, n, windowH);
    const int visible = std::min(n, kComboMaxVisible);
    const float maxPx = std::max(0.0f, static_cast<float>((n - visible) * kComboItemH));
    const float scrollPx = state.lastOpen == state.open
                               ? std::clamp(state.scrollPx, 0.0f, maxPx)
                               : std::clamp(static_cast<float>(state.scroll * kComboItemH), 0.0f, maxPx);

    r.FillRectScreen(list.x + 3, list.y + 4, list.w, list.h, {0, 0, 0, 120});
    FillVerticalGradient(r, list, Mix(color::PanelRaised, color::AccentDeep, 0.14f), color::PanelSunken);
    DrawPanelFrame(r, list, color::Accent);

    const UiRect clip {list.x + 3, list.y + 3, std::max(0, list.w - (n > visible ? 14 : 6)), std::max(0, list.h - 6)};
    const int scrollOff = static_cast<int>(std::lround(scrollPx));
    r.SetClipRect({clip.x, clip.y, clip.w, clip.h});
    for (int i = 0; i < n; ++i)
    {
        const UiRect item {clip.x, list.y + 3 + i * kComboItemH - scrollOff, clip.w, kComboItemH};
        if (item.Bottom() <= clip.y || item.y >= clip.Bottom())
            continue;
        const bool off = disabled && static_cast<std::size_t>(i) < disabled->size()
                         && (*disabled)[static_cast<std::size_t>(i)] && i != current;
        const bool hot = clip.Contains(mouseX, mouseY) && item.Contains(mouseX, mouseY) && !off;
        if (hot)
            SoundRequests::MarkHover();
        if (i == current)
        {
            FillHorizontalGradient(
                r, item, Mix(color::AccentDeep, color::Accent, 0.25f), WithAlpha(color::AccentDeep, 140));
            r.FillRectScreen(item.x, item.y + 2, 3, std::max(0, item.h - 4), color::Accent);
        }
        else if (hot)
        {
            FillHorizontalGradient(r, item, Mix(color::Hover, color::AccentDeep, 0.22f), color::Panel);
            r.FillRectScreen(item.x, item.y + 4, 2, std::max(0, item.h - 8), color::BorderStrong);
        }
        int textOffset = 10;
        if (swatches && static_cast<std::size_t>(i) < swatches->size())
        {
            SDL_Color sw = (*swatches)[static_cast<std::size_t>(i)];
            if (off)
                sw.a = 110;
            const UiRect box {item.x + 8, item.CenterY() - 8, 16, 16};
            r.FillRectScreen(box.x, box.y, box.w, box.h, sw);
            r.DrawRectScreen(box.x, box.y, box.w, box.h, off ? color::Border : color::BorderStrong);
            textOffset = 30;
        }
        const SDL_Color textColor
            = i == current ? color::Accent : (off ? color::TextMuted : (hot ? color::White : color::Text));
        font.DrawString(
            FitText(font, items[static_cast<std::size_t>(i)], FONT_BODY, std::max(0, item.w - textOffset - 10)),
            item.x + textOffset,
            item.CenterY(),
            FONT_BODY,
            textColor,
            false,
            Anchor::LEFT | Anchor::VCENTER);
    }
    r.ResetClipRect();
    if (n > visible)
    {
        const UiRect track {list.Right() - 9, list.y + 4, 5, std::max(0, list.h - 8)};
        DrawScrollBar(
            r,
            track,
            scrollPx,
            static_cast<float>(n * kComboItemH),
            static_cast<float>(visible * kComboItemH),
            OnScrollTrack(track, mouseX, mouseY));
    }
}

bool ComboUpdate(
    ComboState &state,
    const UiRect &field,
    int itemCount,
    int windowH,
    int mx,
    int my,
    bool clicked,
    int wheel,
    int &picked,
    bool down,
    bool pressed,
    int pressX,
    int pressY)
{
    picked = -1;
    if (state.open < 0)
    {
        state.lastOpen = -1;
        return false;
    }
    const UiRect list = ComboListRect(field, itemCount, windowH);
    const int visible = std::min(itemCount, kComboMaxVisible);
    const float maxPx = std::max(0.0f, static_cast<float>((itemCount - visible) * kComboItemH));
    if (state.lastOpen != state.open)
    {
        state.lastOpen = state.open;
        state.scrollPx = state.targetPx = std::clamp(static_cast<float>(state.scroll * kComboItemH), 0.0f, maxPx);
        state.velPx = 0.0f;
        state.dragging = state.dragMoved = false;
        state.lastTickMs = SDL_GetTicks();
    }
    const bool wasDragging = state.dragging;
    StepVerticalKineticScroll(
        state.scrollPx,
        state.targetPx,
        state.velPx,
        state.dragging,
        state.dragMoved,
        state.dragStartY,
        state.dragLastY,
        state.lastTickMs,
        list,
        maxPx,
        kComboItemH * 2,
        mx,
        my,
        down,
        pressed,
        pressX,
        pressY,
        wheel,
        false,
        &field);
    state.scroll = static_cast<int>(state.scrollPx) / kComboItemH;

    if (wheel != 0 && (list.Contains(mx, my) || field.Contains(mx, my)))
        return true;
    if (wasDragging && !state.dragging && state.dragMoved)
    {
        state.dragMoved = false;
        return true;
    }
    if (!clicked)
        return false;
    if (list.Contains(mx, my))
    {
        const int yInList = (my - (list.y + 3)) + static_cast<int>(std::lround(state.scrollPx));
        const int index = yInList / kComboItemH;
        if (yInList >= 0 && index >= 0 && index < itemCount)
            picked = index;
    }
    state.open = -1; // a pick or a click elsewhere closes the list, and the click is not used for anything else
    state.lastOpen = -1;
    state.scroll = 0;
    state.scrollPx = state.targetPx = state.velPx = 0.0f;
    return true;
}

void DrawLoadingScreen(
    Renderer &r,
    BitmapFontRenderer &font,
    int windowW,
    int windowH,
    std::string_view title,
    std::string_view status,
    float progress,
    Uint32 ticks,
    SDL_Texture *preview,
    int previewW,
    int previewH)
{
    FillVerticalGradient(r, {0, 0, windowW, windowH}, Mix(color::Backdrop, color::PanelRaised, 0.55f), color::Backdrop);
    const int cx = windowW / 2, cy = windowH / 2;

    if (preview && previewW > 0 && previewH > 0)
    {
        // A map is loading: its name on top, its picture in the middle, the bar and the state under it
        font.DrawString(
            FitText(font, title, FONT_H1, windowW - 80),
            cx,
            56,
            FONT_H1,
            color::Text,
            true,
            Anchor::HCENTER | Anchor::VCENTER);
        r.FillRectScreen(cx - 60, 86, 120, 3, color::Accent);
        const int maxW = std::min(windowW - 120, 640), maxH = std::max(120, windowH - 300);
        const float scale = std::min(static_cast<float>(maxW) / previewW, static_cast<float>(maxH) / previewH);
        const int w = std::max(1, static_cast<int>(previewW * scale)),
                  h = std::max(1, static_cast<int>(previewH * scale));
        const UiRect frame {cx - w / 2 - 6, 110, w + 12, h + 12};
        r.FillRectScreen(frame.x, frame.y, frame.w, frame.h, color::PanelSunken);
        r.DrawRectScreen(frame.x, frame.y, frame.w, frame.h, color::BorderStrong);
        const SDL_FRect dst {
            static_cast<float>(frame.x + 6),
            static_cast<float>(frame.y + 6),
            static_cast<float>(w),
            static_cast<float>(h)};
        SDL_RenderTexture(r.GetSdlRenderer(), preview, nullptr, &dst);
        const int barW = std::min(frame.w, 640), barH = 8;
        const UiRect bar {cx - barW / 2, frame.Bottom() + 32, barW, barH};
        r.FillRectScreen(bar.x, bar.y, bar.w, bar.h, color::PanelSunken);
        if (progress >= 0.0f)
        {
            const int fw = static_cast<int>((bar.w - 2) * std::clamp(progress, 0.0f, 1.0f));
            if (fw > 0)
                FillHorizontalGradient(r, {bar.x + 1, bar.y + 1, fw, bar.h - 2}, color::AccentDeep, color::Accent);
        }
        r.DrawRectScreen(bar.x, bar.y, bar.w, bar.h, color::Border);
        // The turning ring beside the state text
        const int head = static_cast<int>((ticks / 90) % 12);
        const int tx = cx - 90;
        for (int i = 0; i < 12; ++i)
        {
            const float a
                = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / 12 - static_cast<float>(M_PI) * 0.5f;
            const Uint8 alpha = static_cast<Uint8>(255 - ((head - i + 12) % 12) * 19);
            r.FillRectScreen(
                tx + static_cast<int>(std::cos(a) * 9.0f) - 1,
                bar.y + 34 + static_cast<int>(std::sin(a) * 9.0f) - 1,
                3,
                3,
                WithAlpha(color::Accent, alpha));
        }
        font.DrawString(
            FitText(font, status, FONT_BODY, windowW - 200),
            cx - 66,
            bar.y + 34,
            FONT_BODY,
            color::TextDim,
            false,
            Anchor::LEFT | Anchor::VCENTER);
        return;
    }

    // The name of the game, with an accent line under it
    font.DrawString(title, cx, cy - 96, FONT_HERO, color::Text, true, Anchor::HCENTER | Anchor::VCENTER);
    r.FillRectScreen(cx - 60, cy - 58, 120, 3, color::Accent);

    // Turning indicator: a ring of squares, the head bright and a tail fading behind it
    constexpr int kDots = 12;
    const int head = static_cast<int>((ticks / 90) % kDots);
    for (int i = 0; i < kDots; ++i)
    {
        const float a
            = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / kDots - static_cast<float>(M_PI) * 0.5f;
        const int behind = (head - i + kDots) % kDots;
        const Uint8 alpha = static_cast<Uint8>(255 - behind * 19);
        const int px = cx + static_cast<int>(std::cos(a) * 20.0f) - 2,
                  py = cy - 8 + static_cast<int>(std::sin(a) * 20.0f) - 2;
        r.FillRectScreen(px, py, 5, 5, WithAlpha(color::Accent, alpha));
    }

    // Progress bar
    const int barW = std::min(440, windowW * 6 / 10), barH = 8;
    const UiRect bar {cx - barW / 2, cy + 44, barW, barH};
    r.FillRectScreen(bar.x, bar.y, bar.w, bar.h, color::PanelSunken);
    if (progress >= 0.0f)
    {
        const int fw = static_cast<int>((bar.w - 2) * std::clamp(progress, 0.0f, 1.0f));
        if (fw > 0)
            FillHorizontalGradient(r, {bar.x + 1, bar.y + 1, fw, bar.h - 2}, color::AccentDeep, color::Accent);
    }
    else
    {
        // Unknown length: a piece sliding to and fro
        const int seg = bar.w / 4;
        const float t = static_cast<float>(ticks % 1600) / 800.0f;
        const float pos = t < 1.0f ? t : 2.0f - t;
        const int sx = bar.x + 1 + static_cast<int>((bar.w - 2 - seg) * pos);
        FillHorizontalGradient(r, {sx, bar.y + 1, seg, bar.h - 2}, color::AccentDeep, color::Accent);
    }
    r.DrawRectScreen(bar.x, bar.y, bar.w, bar.h, color::Border);

    // What is going on
    font.DrawString(
        FitText(font, status, FONT_BODY, windowW - 80),
        cx,
        bar.y + 34,
        FONT_BODY,
        color::TextDim,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
    if (progress >= 0.0f)
    {
        font.DrawString(
            std::to_string(static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f + 0.5f)) + "%",
            cx,
            bar.y - 14,
            FONT_SMALL_BOLD,
            color::TextMuted,
            false,
            Anchor::HCENTER | Anchor::VCENTER);
    }
}

void DrawCaption(BitmapFontRenderer &font, Renderer &r, int x, int y, std::string_view text, SDL_Color tick)
{
    r.FillRectScreen(x, y + 3, 3, 10, tick);
    font.DrawString(text, x + 9, y, FONT_SMALL_BOLD, color::TextDim, false);
}

std::string FitText(const BitmapFontRenderer &font, std::string_view text, int scale, int maxWidth)
{
    if (maxWidth <= 0)
        return {};
    if (font.GetTextWidth(text, scale) <= maxWidth)
        return std::string(text);

    static const std::string kEllipsis = "\xE2\x80\xA6";
    std::string s(text);
    while (!s.empty())
    {
        // Drop one UTF-8 code point from the end
        std::size_t cut = s.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s.erase(cut);
        if (font.GetTextWidth(s + kEllipsis, scale) <= maxWidth)
            return s + kEllipsis;
    }
    return {};
}

UiRect CenteredRect(int windowW, int windowH, int w, int h, int margin)
{
    w = std::min(w, windowW - 2 * margin);
    h = std::min(h, windowH - 2 * margin);
    return {(windowW - w) / 2, (windowH - h) / 2, w, h};
}

} // namespace ui
} // namespace OpenArtOfWar
