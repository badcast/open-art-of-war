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
    const SDL_Color flat = Mix(top, bottom, 0.5f);
    if (flat.a == 0)
        return;
    r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, flat);
}

void FillHorizontalGradient(Renderer &r, const UiRect &rect, SDL_Color left, SDL_Color right)
{
    if (rect.h <= 0 || rect.w <= 0)
        return;
    const SDL_Color flat = Mix(left, right, 0.5f);
    if (flat.a == 0)
        return;
    r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, flat);
}

void DrawPanelFrame(Renderer &r, const UiRect &rect, SDL_Color border)
{
    if (rect.w <= 0 || rect.h <= 0 || border.a == 0)
        return;
    r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, border);
    if (rect.w > 4 && rect.h > 4)
        r.DrawRectScreen(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2, WithAlpha(color::PanelSunken, border.a));
}

namespace
{
bool g_flatSkin = false;
const SDL_Color kSlate {12, 16, 22, 255};
const SDL_Color kSlateRim {160, 172, 190, 255};
} // namespace

void SetFlatSkin(bool on)
{
    g_flatSkin = on;
}

bool FlatSkin()
{
    return g_flatSkin;
}

float ToastSlide(float elapsedSeconds, float totalSeconds, float inSeconds, float outSeconds)
{
    if (elapsedSeconds < 0.0f || elapsedSeconds >= totalSeconds)
        return 0.0f;
    float t = 1.0f;
    if (elapsedSeconds < inSeconds)
    {
        const float k = elapsedSeconds / std::max(0.001f, inSeconds);
        t = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k); // eases out: fast, then settles
    }
    else if (totalSeconds - elapsedSeconds < outSeconds)
    {
        const float k = (totalSeconds - elapsedSeconds) / std::max(0.001f, outSeconds);
        t = k * k * k; // eases in: leaves slowly, then fast
    }
    return std::clamp(t, 0.0f, 1.0f);
}

void DrawToast(
    Renderer &r,
    BitmapFontRenderer &font,
    int x,
    int y,
    int height,
    int maxWidth,
    std::string_view text,
    float slide,
    SDL_Color accent)
{
    if (slide <= 0.0f || text.empty())
        return;
    const int pad = std::max(8, height / 3);
    const int width = std::min(maxWidth, font.GetTextWidth(std::string(text), FONT_BODY_BOLD) + 2 * pad + 4);
    const int drawX = x - static_cast<int>(std::lround((1.0f - slide) * static_cast<float>(width + x)));
    r.FillRectScreen(drawX, y, width, height, {10, 13, 18, 228});
    r.DrawRectScreen(drawX, y, width, height, {160, 172, 190, 150});
    r.FillRectScreen(drawX + 1, y + 1, 4, height - 2, accent);
    font.DrawString(
        FitText(font, text, FONT_BODY_BOLD, std::max(20, width - 2 * pad - 4)),
        drawX + 4 + pad,
        y + height / 2,
        FONT_BODY_BOLD,
        color::Text,
        false,
        Anchor::LEFT | Anchor::VCENTER);
}

void DrawPanel(Renderer &r, const UiRect &rect, SDL_Color fill, SDL_Color border)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    if (g_flatSkin)
    {
        if (fill.a > 0)
        {
            SDL_Color flat = Mix(kSlate, fill, 0.22f);
            flat.a = fill.a;
            r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, flat);
        }
        SDL_Color rim = Mix(kSlateRim, border, 0.25f);
        rim.a = 150;
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, rim);
        return;
    }
    if (fill.a > 0)
        r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
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

    if (g_flatSkin)
    {
        SDL_Color fill = active ? Mix(kSlate, accent, 0.42f) : Mix(kSlate, accent, hovered ? 0.22f : 0.08f);
        fill.a = active ? 224 : (hovered ? 215 : 195);
        SDL_Color rim = Mix(accent, color::White, active ? 0.50f : (hovered ? 0.55f : 0.30f));
        rim.a = active ? 245 : (hovered ? 240 : 190);
        r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, rim);
        if (active && rect.h > 4)
            r.FillRectScreen(rect.x + 1, rect.Bottom() - 3, std::max(0, rect.w - 2), 2, WithAlpha(Mix(accent, color::White, 0.65f), 245));
        if (font && !label.empty())
        {
            const std::string drawn = FitText(*font, label, FONT_BODY_BOLD, std::max(0, rect.w - 16));
            font->DrawString(
                drawn, rect.CenterX(), rect.CenterY(), FONT_BODY_BOLD, color::White, true, Anchor::HCENTER | Anchor::VCENTER);
        }
        return;
    }

    const SDL_Color topFill = active
                                  ? Mix(color::PanelRaised, accent, 0.38f)
                                  : (hovered ? Mix(color::Hover, color::AccentDeep, 0.42f)
                                             : Mix(color::PanelRaised, color::AccentDeep, 0.18f));
    const SDL_Color botFill = active
                                  ? Mix(color::PanelSunken, color::AccentDeep, 0.72f)
                                  : (hovered ? Mix(color::PanelSunken, color::AccentDeep, 0.28f) : color::PanelSunken);
    const SDL_Color border = active
                                 ? Mix(accent, color::White, 0.18f)
                                 : (hovered ? Mix(color::BorderStrong, accent, 0.48f)
                                            : Mix(color::Border, color::BorderStrong, 0.55f));
    const SDL_Color corners = active
                                  ? Mix(accent, color::White, 0.42f)
                                  : (hovered ? Mix(accent, color::BorderStrong, 0.35f)
                                             : WithAlpha(color::BorderStrong, 170));

    DrawButtonChrome(r, rect, topFill, botFill, border, {0, 0, 0, 0}, corners);

    if (active && rect.w > 2 && rect.h > 4)
    {
        r.FillRectScreen(rect.x + 1, rect.Bottom() - 3, rect.w - 2, 3, accent);
        r.FillRectScreen(rect.x + 1, rect.Bottom() - 3, rect.w - 2, 1, Mix(accent, color::White, 0.35f));
    }
    else if (hovered && rect.w > 2 && rect.h > 4)
    {
        r.FillRectScreen(rect.x + 1, rect.Bottom() - 2, rect.w - 2, 2, WithAlpha(accent, 120));
    }

    if (font && !label.empty())
    {
        const SDL_Color textColor = active ? color::White : (hovered ? color::Text : color::TextDim);
        const std::string drawn = FitText(*font, label, FONT_BODY_BOLD, std::max(0, rect.w - 16));
        const int textY = active ? rect.CenterY() - 1 : rect.CenterY();
        font->DrawString(
            drawn, rect.CenterX(), textY, FONT_BODY_BOLD, textColor, true, Anchor::HCENTER | Anchor::VCENTER);
    }
}

CompositeFit FitCompositeInRect(const UiRect &box, int pad, float minX, float minY, float maxX, float maxY)
{
    const float compW = std::max(1.0f, maxX - minX);
    const float compH = std::max(1.0f, maxY - minY);
    const float innerW = static_cast<float>(std::max(1, box.w - pad * 2));
    const float innerH = static_cast<float>(std::max(1, box.h - pad * 2));
    const float scale = std::min(innerW / compW, innerH / compH);
    const float scaledW = compW * scale;
    const float scaledH = compH * scale;
    const int drawX = static_cast<int>(
        std::lround(static_cast<float>(box.x) + (static_cast<float>(box.w) - scaledW) * 0.5f - minX * scale));
    const int drawY = static_cast<int>(
        std::lround(static_cast<float>(box.y) + (static_cast<float>(box.h) - scaledH) * 0.5f - minY * scale));
    return {drawX, drawY, scale};
}

void DrawCompositeFitted(Renderer &r, int compId, const UiRect &box, int pad, Faction faction)
{
    if (compId < 0 || box.w <= 0 || box.h <= 0)
        return;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    if (!r.GetCompositeBounds(compId, minX, minY, maxX, maxY))
        return;
    const CompositeFit fit = FitCompositeInRect(box, pad, minX, minY, maxX, maxY);
    r.DrawCompositeScreen(compId, fit.x, fit.y, fit.scale, faction);
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
    r.FillRectScreen(
        rect.x + 4,
        rect.y + 4,
        innerW,
        titleH - 4,
        WithAlpha(Mix(color::PanelRaised, color::AccentDeep, 0.25f), 220));
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

} // namespace

void DrawButtonChrome(
    Renderer &r,
    const UiRect &rect,
    SDL_Color topFill,
    SDL_Color bottomFill,
    SDL_Color border,
    SDL_Color accentBar,
    [[maybe_unused]] SDL_Color cornerColor,
    Uint8 opacity,
    bool horizontalGradient)
{
    if (rect.w <= 0 || rect.h <= 0 || opacity == 0)
        return;

    const SDL_Color topCol = ScaleAlpha(topFill, opacity);
    const SDL_Color botCol = ScaleAlpha(bottomFill, opacity);
    const SDL_Color rimCol = ScaleAlpha(border, opacity);
    const SDL_Color barCol = ScaleAlpha(accentBar, opacity);

    const SDL_Color flatCol = Mix(topCol, botCol, 0.5f);
    if (flatCol.a > 0)
        r.FillRectScreen(rect.x, rect.y, rect.w, rect.h, flatCol);

    if (!horizontalGradient && rect.w >= 6 && rect.h >= 6)
    {
        SDL_Color shadow {4, 6, 10, static_cast<Uint8>(static_cast<unsigned>(botCol.a) * 140 / 255)};
        r.FillRectScreen(rect.x + 1, rect.Bottom() - 2, rect.w - 2, 1, shadow);
    }

    if (rimCol.a > 0 && !horizontalGradient)
        r.DrawRectScreen(rect.x, rect.y, rect.w, rect.h, rimCol);

    if (barCol.a > 0 && rect.w >= 12 && rect.h >= 6)
    {
        const int barW = horizontalGradient ? 4 : 3;
        r.FillRectScreen(rect.x + 1, rect.y + 1, barW, rect.h - 2, barCol);
    }
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
                = hovered ? Mix(color::AccentDeep, color::Accent, 0.36f) : Mix(color::PanelRaised, color::AccentDeep, 0.52f);
            botFill = hovered ? Mix(color::PanelRaised, color::AccentDeep, 0.62f)
                              : Mix(color::PanelSunken, color::AccentDeep, 0.30f);
            border = hovered ? Mix(color::Accent, color::White, 0.25f) : Mix(color::BorderStrong, color::Accent, 0.38f);
            corners
                = hovered ? Mix(color::Accent, color::White, 0.40f) : Mix(color::Accent, color::BorderStrong, 0.45f);
            text = hovered ? color::White : Mix(color::Text, color::White, 0.25f);
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

    if (g_flatSkin && style != ButtonStyle::Ghost)
    {
        // The touch interface's plates: the army colour darkened for the fill, lightened for the rim
        const SDL_Color army = color::Accent;
        const float luma = 0.299f * army.r + 0.587f * army.g + 0.114f * army.b;
        const SDL_Color rich = luma > 165.0f ? Mix(army, color::PanelSunken, 0.32f) : army;
        SDL_Color fill, rim;
        if (!enabled)
        {
            fill = WithAlpha(Mix(rich, color::PanelSunken, 0.72f), 140);
            rim = WithAlpha(Mix(rich, color::Border, 0.50f), 140);
        }
        else if (style == ButtonStyle::Danger)
        {
            fill = hovered ? SDL_Color {186, 42, 34, 224} : SDL_Color {142, 34, 28, 200};
            rim = hovered ? SDL_Color {255, 118, 106, 242} : SDL_Color {226, 86, 76, 215};
        }
        else if (style == ButtonStyle::Primary)
        {
            fill = WithAlpha(hovered ? Mix(rich, color::White, 0.14f) : Mix(rich, color::PanelSunken, 0.10f), 224);
            rim = WithAlpha(Mix(army, color::White, hovered ? 0.72f : 0.50f), 245);
        }
        else
        {
            fill = WithAlpha(Mix(rich, color::PanelSunken, hovered ? 0.24f : 0.42f), hovered ? 215 : 195);
            rim = WithAlpha(Mix(army, color::White, hovered ? 0.55f : 0.35f), hovered ? 240 : 215);
        }
        topFill = botFill = fill;
        border = rim;
        corners = {0, 0, 0, 0};
        text = enabled ? WithAlpha(color::White, 240) : WithAlpha(color::White, 130);
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

    const SDL_Color boxFill = Mix(topFill, botFill, 0.5f);
    if (boxFill.a > 0)
        r.FillRectScreen(box.x, box.y, box.w, box.h, boxFill);
    r.DrawRectScreen(box.x, box.y, box.w, box.h, border);

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
void StepVerticalKineticScroll(
    float &offset,
    float &target,
    float &velocity,
    bool &dragging,
    bool &dragMoved,
    bool &hasVelocity,
    float &dragStartY,
    float &dragLastY,
    float &dragIdleS,
    Uint64 &lastTickNs,
    const UiRect &area,
    float maxScroll,
    int stepPx,
    int mx,
    int my,
    bool down,
    bool pressed,
    int pressX,
    int pressY,
    float wheel,
    bool allowWheelOutside = false,
    const UiRect *extraWheelRect = nullptr)
{
    KineticScroll2D k;
    k.offsetY = offset;
    k.targetY = target;
    k.velY = velocity;
    k.dragging = dragging;
    k.dragMoved = dragMoved;
    k.hasVelocity = hasVelocity;
    k.dragStartY = dragStartY;
    k.dragLastY = dragLastY;
    k.dragIdleS = dragIdleS;
    k.lastTickNs = lastTickNs;

    StepKineticScroll2D(
        k,
        area,
        0.0f,
        maxScroll,
        0.0f,
        static_cast<float>(stepPx),
        mx,
        my,
        down,
        pressed,
        pressX,
        pressY,
        0.0f,
        wheel,
        true,
        allowWheelOutside,
        extraWheelRect);

    offset = k.offsetY;
    target = k.targetY;
    velocity = k.velY;
    dragging = k.dragging;
    dragMoved = k.dragMoved;
    hasVelocity = k.hasVelocity;
    dragStartY = k.dragStartY;
    dragLastY = k.dragLastY;
    dragIdleS = k.dragIdleS;
    lastTickNs = k.lastTickNs;
}
} // namespace

void StepKineticScroll2D(
    KineticScroll2D &scroll,
    const UiRect &area,
    float maxScrollX,
    float maxScrollY,
    float stepPxX,
    float stepPxY,
    int mx,
    int my,
    bool down,
    bool pressed,
    int pressX,
    int pressY,
    float wheelX,
    float wheelY,
    bool allowStart,
    bool allowWheelOutside,
    const UiRect *extraWheelRect)
{
    constexpr float kSlop = 12.0f;
    constexpr float kGlideMax = 6000.0f;
    constexpr float kGlideStop = 30.0f;
    constexpr float kGlideFriction = 3.5f;
    constexpr float kWheelEase = 18.0f;
    constexpr float kVelBlendTau = 0.045f;
    constexpr float kHoldGraceS = 0.035f;
    constexpr float kHoldDecayTau = 0.018f;

    const Uint64 nowNs = SDL_GetTicksNS();
    const float dtS
        = scroll.lastTickNs == 0
              ? 0.0f
              : std::clamp(static_cast<float>(nowNs - scroll.lastTickNs) * 1e-9f, 0.0f, 0.1f);
    scroll.lastTickNs = nowNs;

    const bool canScroll = maxScrollX > 0.0f || maxScrollY > 0.0f;
    const bool wheelHit
        = allowWheelOutside || area.Contains(mx, my) || (extraWheelRect && extraWheelRect->Contains(mx, my));
    if (!scroll.dragging && wheelHit)
    {
        if (wheelX != 0.0f && maxScrollX > 0.0f)
        {
            scroll.velX = 0.0f;
            scroll.hasVelocity = scroll.velY != 0.0f;
            scroll.targetX = std::clamp(scroll.targetX - wheelX * stepPxX, 0.0f, maxScrollX);
        }
        if (wheelY != 0.0f && maxScrollY > 0.0f)
        {
            scroll.velY = 0.0f;
            scroll.hasVelocity = scroll.velX != 0.0f;
            scroll.targetY = std::clamp(scroll.targetY - wheelY * stepPxY, 0.0f, maxScrollY);
        }
    }

    if (pressed && down && allowStart && canScroll && area.Contains(pressX, pressY))
    {
        scroll.velX = scroll.velY = 0.0f;
        scroll.hasVelocity = false;
        scroll.dragging = true;
        scroll.dragMoved = false;
        scroll.dragStartX = scroll.dragLastX = static_cast<float>(pressX);
        scroll.dragStartY = scroll.dragLastY = static_cast<float>(pressY);
        scroll.dragIdleS = 0.0f;
    }

    bool movedThisStep = false;
    if (scroll.dragging)
    {
        if (!scroll.dragMoved)
        {
            const float distX = maxScrollX > 0.0f ? (static_cast<float>(mx) - scroll.dragStartX) : 0.0f;
            const float distY = maxScrollY > 0.0f ? (static_cast<float>(my) - scroll.dragStartY) : 0.0f;
            const float dist = std::hypot(distX, distY);
            if (dist > kSlop)
            {
                scroll.dragMoved = true;
                scroll.dragIdleS = 0.0f;
                const float scale = kSlop / dist;
                scroll.dragLastX = scroll.dragStartX + distX * scale;
                scroll.dragLastY = scroll.dragStartY + distY * scale;
            }
        }
        if (scroll.dragMoved)
        {
            const float dx = maxScrollX > 0.0f ? (static_cast<float>(mx) - scroll.dragLastX) : 0.0f;
            const float dy = maxScrollY > 0.0f ? (static_cast<float>(my) - scroll.dragLastY) : 0.0f;
            scroll.dragLastX = static_cast<float>(mx);
            scroll.dragLastY = static_cast<float>(my);
            movedThisStep = (dx != 0.0f || dy != 0.0f);
            if (movedThisStep)
            {
                scroll.offsetX -= dx;
                scroll.offsetY -= dy;
                scroll.targetX = scroll.offsetX;
                scroll.targetY = scroll.offsetY;
                const float moveDt = std::clamp(dtS + scroll.dragIdleS, 0.0f, 0.1f);
                scroll.dragIdleS = 0.0f;
                if (moveDt > 0.0f)
                {
                    const float instVx = std::clamp(-dx / moveDt, -kGlideMax, kGlideMax);
                    const float instVy = std::clamp(-dy / moveDt, -kGlideMax, kGlideMax);
                    if (!scroll.hasVelocity)
                    {
                        scroll.velX = instVx;
                        scroll.velY = instVy;
                        scroll.hasVelocity = true;
                    }
                    else
                    {
                        const float k = std::min(1.0f, moveDt / kVelBlendTau);
                        scroll.velX += (instVx - scroll.velX) * k;
                        scroll.velY += (instVy - scroll.velY) * k;
                    }
                }
            }
            else if (down && dtS > 0.0f && scroll.hasVelocity)
            {
                scroll.dragIdleS += dtS;
                if (scroll.dragIdleS >= kHoldGraceS)
                {
                    const float decay = std::exp(-dtS / kHoldDecayTau);
                    scroll.velX *= decay;
                    scroll.velY *= decay;
                }
            }
        }
        if (!down)
        {
            scroll.dragging = false;
            scroll.dragIdleS = 0.0f;
        }
    }

    if (!scroll.dragging)
    {
        const float speed = std::hypot(scroll.velX, scroll.velY);
        if (speed >= kGlideStop && canScroll)
        {
            if (!movedThisStep && dtS > 0.0f)
            {
                scroll.offsetX += scroll.velX * dtS;
                scroll.offsetY += scroll.velY * dtS;
                scroll.targetX = scroll.offsetX;
                scroll.targetY = scroll.offsetY;
                const float keep = std::exp(-dtS * kGlideFriction);
                scroll.velX *= keep;
                scroll.velY *= keep;
            }
        }
        else
        {
            scroll.velX = scroll.velY = 0.0f;
            scroll.hasVelocity = false;
            auto easeAxis = [&](float &offset, float target) {
                if (std::abs(target - offset) > 0.35f)
                {
                    if (dtS > 0.0f)
                        offset += (target - offset) * (1.0f - std::exp(-dtS * kWheelEase));
                }
                else
                {
                    offset = target;
                }
            };
            easeAxis(scroll.offsetX, scroll.targetX);
            easeAxis(scroll.offsetY, scroll.targetY);
        }
    }

    const float clampedX = std::clamp(scroll.offsetX, 0.0f, maxScrollX);
    if (clampedX != scroll.offsetX)
    {
        scroll.offsetX = clampedX;
        scroll.velX = 0.0f;
    }
    const float clampedY = std::clamp(scroll.offsetY, 0.0f, maxScrollY);
    if (clampedY != scroll.offsetY)
    {
        scroll.offsetY = clampedY;
        scroll.velY = 0.0f;
    }
    if (scroll.velX == 0.0f && scroll.velY == 0.0f)
        scroll.hasVelocity = false;
    scroll.targetX = std::clamp(scroll.targetX, 0.0f, maxScrollX);
    scroll.targetY = std::clamp(scroll.targetY, 0.0f, maxScrollY);
}

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
    float wheel,
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
        scroll.StopGlide();
        scroll.dragging = false;
        scroll.dragMoved = false;
        scroll.dragIdleS = 0.0f;
    }
    if (scroll.barDrag)
    {
        const float pos
            = ScrollPosFromMouse(trackRect, my, static_cast<float>(contentH), static_cast<float>(listRect.h));
        scroll.offset = scroll.target = std::clamp(pos, 0.0f, maxScroll);
        scroll.lastTickNs = SDL_GetTicksNS();
        clicked = false;
        if (!down)
            scroll.barDrag = false;
        return;
    }

    const bool wasDragging = scroll.dragging;
    StepVerticalKineticScroll(
        scroll.offset,
        scroll.target,
        scroll.velocity,
        scroll.dragging,
        scroll.dragMoved,
        scroll.hasVelocity,
        scroll.dragStartY,
        scroll.dragLastY,
        scroll.dragIdleS,
        scroll.lastTickNs,
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
    if (scroll.dragMoved)
        clicked = false;
    if (wasDragging && !scroll.dragging && scroll.dragMoved)
        scroll.dragMoved = false;
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
    r.FillRectScreen(list.x, list.y, list.w, list.h, Mix(color::PanelRaised, color::PanelSunken, 0.55f));
    DrawPanelFrame(r, list, color::Accent);

    const UiRect clip {list.x + 3, list.y + 3, std::max(0, list.w - (n > visible ? 14 : 6)), std::max(0, list.h - 6)};
    const int scrollOff = static_cast<int>(std::lround(scrollPx));
    r.SetClipRect({clip.x, clip.y, clip.w, clip.h});
    for (int i = 0; i < n; ++i)
    {
        const UiRect item {clip.x, list.y + 3 + i * kComboItemH - scrollOff, clip.w, kComboItemH};
        const int topY = std::max(item.y, clip.y);
        const int botY = std::min(item.Bottom(), clip.Bottom());
        if (botY <= topY)
            continue;
        const UiRect bgRect {item.x, topY, item.w, botY - topY};
        const bool off = disabled && static_cast<std::size_t>(i) < disabled->size()
                         && (*disabled)[static_cast<std::size_t>(i)] && i != current;
        const bool hot = clip.Contains(mouseX, mouseY) && item.Contains(mouseX, mouseY) && !off;
        if (hot)
            SoundRequests::MarkHover();
        if (i == current)
        {
            r.FillRectScreen(bgRect.x, bgRect.y, bgRect.w, bgRect.h, Mix(color::AccentDeep, color::Accent, 0.25f));
            r.FillRectScreen(bgRect.x, bgRect.y, 3, bgRect.h, color::Accent);
        }
        else if (hot)
        {
            r.FillRectScreen(bgRect.x, bgRect.y, bgRect.w, bgRect.h, Mix(color::Hover, color::AccentDeep, 0.22f));
            r.FillRectScreen(bgRect.x, bgRect.y, 2, bgRect.h, color::BorderStrong);
        }
        if (item.CenterY() - 8 < clip.y || item.CenterY() + 8 > clip.Bottom())
            continue;
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
    float wheel,
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
        state.dragging = state.dragMoved = state.hasVelocity = false;
        state.dragIdleS = 0.0f;
        state.lastTickNs = SDL_GetTicksNS();
    }
    const bool wasDragging = state.dragging;
    StepVerticalKineticScroll(
        state.scrollPx,
        state.targetPx,
        state.velPx,
        state.dragging,
        state.dragMoved,
        state.hasVelocity,
        state.dragStartY,
        state.dragLastY,
        state.dragIdleS,
        state.lastTickNs,
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

    if (wheel != 0.0f && (list.Contains(mx, my) || field.Contains(mx, my)))
        return true;
    if (wasDragging && !state.dragging && state.dragMoved)
    {
        state.dragMoved = false;
        return true;
    }
    if (state.dragMoved)
        return true;
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
    state.hasVelocity = false;
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
