#include <cmath>
#include "ui/UiKit.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "core/Localization.hpp"
#include "graphics/RenderUtility.hpp"
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
                // neutral (menus)
                {{17, 21, 28, 246}, {25, 30, 40, 255}, {11, 14, 19, 255}, {50, 60, 76, 255}, {78, 92, 114, 255}, {33, 40, 53, 255}, {232, 176, 74, 255}, {120, 84, 30, 255}, {24, 18, 8, 255}},
                // resistance
                {{26, 18, 17, 246}, {38, 26, 24, 255}, {17, 12, 11, 255}, {82, 52, 46, 255}, {124, 78, 68, 255}, {52, 34, 31, 255}, {228, 98, 66, 255}, {116, 40, 26, 255}, {26, 10, 6, 255}},
                // confederation
                {{15, 21, 31, 246}, {22, 31, 46, 255}, {10, 14, 22, 255}, {44, 62, 90, 255}, {70, 98, 136, 255}, {29, 41, 60, 255}, {88, 166, 238, 255}, {28, 70, 122, 255}, {6, 14, 26, 255}},
            };

            SDL_Color readColor(const YAML::Node &n, const char *key, SDL_Color fallback)
            {
                try
                {
                    if(n && n.IsMap() && n[key])
                        return RenderUtility::ParseHexColor(n[key].as<std::string>(), fallback);
                }
                catch(...)
                {
                }
                return fallback;
            }

        } // namespace

        bool loadTheme(const std::string &yamlText)
        {
            try
            {
                const YAML::Node root = YAML::Load(yamlText);
                if(!root.IsMap())
                    return false;
                const char *const names[3] = {"neutral", "resistance", "confederation"};
                for(int i = 0; i < 3; ++i)
                {
                    const YAML::Node n = root["themes"] ? root["themes"][names[i]] : YAML::Node();
                    Theme &t = g_themes[i];
                    t.panel = readColor(n, "panel", t.panel);
                    t.raised = readColor(n, "panel_raised", t.raised);
                    t.sunken = readColor(n, "panel_sunken", t.sunken);
                    t.border = readColor(n, "border", t.border);
                    t.borderStrong = readColor(n, "border_strong", t.borderStrong);
                    t.hover = readColor(n, "hover", t.hover);
                    t.accent = readColor(n, "accent", t.accent);
                    t.accentDeep = readColor(n, "accent_deep", t.accentDeep);
                    t.accentText = readColor(n, "accent_text", t.accentText);
                }
                const YAML::Node c = root["colors"];
                color::Backdrop = readColor(c, "backdrop", color::Backdrop);
                color::Scrim = readColor(c, "scrim", color::Scrim);
                color::Text = readColor(c, "text", color::Text);
                color::TextDim = readColor(c, "text_dim", color::TextDim);
                color::TextMuted = readColor(c, "text_muted", color::TextMuted);
                color::Danger = readColor(c, "danger", color::Danger);
                color::DangerDeep = readColor(c, "danger_deep", color::DangerDeep);
                color::Ok = readColor(c, "ok", color::Ok);
                color::Info = readColor(c, "info", color::Info);
                return true;
            }
            catch(...)
            {
                return false;
            }
        }

        void applyTheme(Faction faction, const SDL_Color *army)
        {
            Theme t = faction == Faction::Resistance ? g_themes[1] : faction == Faction::Confederation ? g_themes[2] : g_themes[0];
            if(army)
            {
                // The army color becomes the accent; panels, borders and hover lean slightly towards it
                const SDL_Color a = *army;
                const SDL_Color black {0, 0, 0, 255};
                const float luma = 0.299f * a.r + 0.587f * a.g + 0.114f * a.b;
                t.accent = {a.r, a.g, a.b, 255};
                t.accentDeep = mix(a, black, 0.55f);
                t.accentDeep.a = 255;
                t.accentText = luma > 140.0f ? SDL_Color {20, 16, 10, 255} : SDL_Color {245, 245, 245, 255};
                t.panel = mix(t.panel, a, 0.07f);
                t.raised = mix(t.raised, a, 0.08f);
                t.sunken = mix(t.sunken, a, 0.05f);
                t.border = mix(t.border, a, 0.30f);
                t.borderStrong = mix(t.borderStrong, a, 0.45f);
                t.hover = mix(t.hover, a, 0.14f);
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

        SDL_Color withAlpha(SDL_Color c, Uint8 a)
        {
            c.a = a;
            return c;
        }

        SDL_Color mix(SDL_Color a, SDL_Color b, float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            auto lerp = [t](Uint8 x, Uint8 y) { return static_cast<Uint8>(x + (y - x) * t); };
            return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
        }

        void fillVerticalGradient(Renderer &r, const UiRect &rect, SDL_Color top, SDL_Color bottom)
        {
            if(rect.h <= 0 || rect.w <= 0)
                return;
            for(int i = 0; i < rect.h; ++i)
            {
                float t = (rect.h > 1) ? static_cast<float>(i) / static_cast<float>(rect.h - 1) : 0.0f;
                SDL_Color c = mix(top, bottom, t);
                r.drawLineScreen(rect.x, rect.y + i, rect.x + rect.w - 1, rect.y + i, c);
            }
        }

        void fillHorizontalGradient(Renderer &r, const UiRect &rect, SDL_Color left, SDL_Color right)
        {
            if(rect.h <= 0 || rect.w <= 0)
                return;
            for(int i = 0; i < rect.w; ++i)
            {
                float t = (rect.w > 1) ? static_cast<float>(i) / static_cast<float>(rect.w - 1) : 0.0f;
                SDL_Color c = mix(left, right, t);
                r.drawLineScreen(rect.x + i, rect.y, rect.x + i, rect.y + rect.h - 1, c);
            }
        }

        void drawPanel(Renderer &r, const UiRect &rect, SDL_Color fill, SDL_Color border)
        {
            r.fillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
            r.drawLineScreen(rect.x + 1, rect.y + 1, rect.right() - 2, rect.y + 1, {255, 255, 255, 14});
            r.drawRectScreen(rect.x, rect.y, rect.w, rect.h, border);
        }

        UiRect drawDialog(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const UiRect &rect, const std::string &title)
        {
            r.fillRectScreen(0, 0, windowW, windowH, color::Scrim);

            // Soft drop shadow
            r.fillRectScreen(rect.x + 6, rect.y + 8, rect.w, rect.h, {0, 0, 0, 90});
            drawPanel(r, rect, color::Panel, color::BorderStrong);

            const int titleH = 52;
            fillVerticalGradient(r, {rect.x + 1, rect.y + 1, rect.w - 2, titleH - 1}, color::PanelRaised, color::Panel);
            r.fillRectScreen(rect.x + 1, rect.y + titleH, rect.w - 2, 1, color::Border);
            r.fillRectScreen(rect.x + 20, rect.y + titleH - 1, 56, 2, color::Accent);
            font.drawString(fitText(font, title, FONT_H2, rect.w - 40), rect.x + 20, rect.y + 14, FONT_H2, color::Text, false);

            return {rect.x + 20, rect.y + titleH + 16, rect.w - 40, rect.h - titleH - 32};
        }

        void drawButton(Renderer &r, BitmapFontRenderer &font, const UiRect &rect, const std::string &label, ButtonStyle style, bool hovered, bool enabled)
        {
            SDL_Color fill = color::PanelRaised;
            SDL_Color border = color::Border;
            SDL_Color text = color::Text;

            switch(style)
            {
                case ButtonStyle::Primary:
                    fill = hovered ? SDL_Color {246, 196, 102, 255} : color::Accent;
                    border = {255, 226, 160, 255};
                    text = color::AccentText;
                    break;
                case ButtonStyle::Secondary:
                    fill = hovered ? color::Hover : color::PanelRaised;
                    border = hovered ? color::Accent : color::BorderStrong;
                    break;
                case ButtonStyle::Danger:
                    fill = hovered ? SDL_Color {110, 38, 32, 255} : color::DangerDeep;
                    border = hovered ? color::Danger : SDL_Color {130, 54, 46, 255};
                    text = {255, 214, 206, 255};
                    break;
                case ButtonStyle::Ghost:
                    fill = hovered ? color::Hover : SDL_Color {0, 0, 0, 0};
                    border = hovered ? color::BorderStrong : SDL_Color {0, 0, 0, 0};
                    text = hovered ? color::Text : color::TextDim;
                    break;
            }
            if(!enabled)
            {
                fill = color::PanelSunken;
                border = color::Border;
                text = color::TextMuted;
            }

            r.fillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
            if(border.a > 0)
                r.drawRectScreen(rect.x, rect.y, rect.w, rect.h, border);
            std::string shown = fitText(font, label, FONT_BODY_BOLD, rect.w - 16);
            font.drawString(shown, rect.centerX(), rect.centerY(), FONT_BODY_BOLD, text, style != ButtonStyle::Primary, Anchor::HCENTER | Anchor::VCENTER);
        }

        void drawProgressBar(Renderer &r, const UiRect &rect, float ratio, SDL_Color fillColor)
        {
            r.fillRectScreen(rect.x, rect.y, rect.w, rect.h, color::PanelSunken);
            int fw = static_cast<int>((rect.w - 2) * std::clamp(ratio, 0.0f, 1.0f));
            if(fw > 0)
                r.fillRectScreen(rect.x + 1, rect.y + 1, fw, rect.h - 2, fillColor);
            r.drawRectScreen(rect.x, rect.y, rect.w, rect.h, color::Border);
        }

        namespace
        {
            UiRect confirmDialogRect(int windowW, int windowH)
            {
                return centeredRect(windowW, windowH, 520, 230);
            }
            UiRect confirmYes(const UiRect &d)
            {
                return {d.x + 24, d.bottom() - 64, (d.w - 60) / 2, 40};
            }
            UiRect confirmNo(const UiRect &d)
            {
                return {d.right() - 24 - (d.w - 60) / 2, d.bottom() - 64, (d.w - 60) / 2, 40};
            }
        } // namespace

        int confirmUpdate(const ConfirmState &state, int windowW, int windowH, int mx, int my, bool clicked, bool enterPressed, bool escPressed)
        {
            if(!state.open)
                return -1;
            const UiRect d = confirmDialogRect(windowW, windowH);
            if(enterPressed || (clicked && confirmYes(d).contains(mx, my)))
                return 1;
            if(escPressed || (clicked && confirmNo(d).contains(mx, my)))
                return 0;
            return -1;
        }

        void drawConfirm(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const ConfirmState &state, int mx, int my)
        {
            if(!state.open)
                return;
            const UiRect d = confirmDialogRect(windowW, windowH);
            const UiRect content = drawDialog(r, font, windowW, windowH, d, state.title);
            font.drawTextWrapped(state.text, content.x + 24, content.y + 18, content.w - 48, FONT_BODY, color::Text, false);
            drawButton(r, font, confirmYes(d), _("YES"), ButtonStyle::Danger, confirmYes(d).contains(mx, my));
            drawButton(r, font, confirmNo(d), _("NO"), ButtonStyle::Secondary, confirmNo(d).contains(mx, my));
        }

        void drawComboField(Renderer &r, BitmapFontRenderer &font, const UiRect &rect, const std::string &value, bool open, bool hovered, bool enabled, SDL_Color valueColor)
        {
            const SDL_Color fill = !enabled ? color::PanelSunken : (open || hovered) ? color::Hover : color::PanelRaised;
            r.fillRectScreen(rect.x, rect.y, rect.w, rect.h, fill);
            r.drawRectScreen(rect.x, rect.y, rect.w, rect.h, open ? color::Accent : (hovered && enabled ? color::BorderStrong : color::Border));
            const bool narrow = rect.w < 100; // a cell of a table
            font.drawString(fitText(font, value, FONT_BODY, rect.w - (narrow ? 26 : 40)), rect.x + (narrow ? 6 : 12), rect.centerY(), FONT_BODY, enabled ? valueColor : color::TextMuted, false, Anchor::LEFT | Anchor::VCENTER);
            // The arrow: a small triangle, pointing up while the list is open
            const SDL_Color arrow = enabled ? (open ? color::Accent : color::TextDim) : color::TextMuted;
            const int ax = rect.right() - (narrow ? 11 : 20), ay = rect.centerY();
            for(int i = 0; i < 5; ++i)
            {
                const int half = open ? i : 4 - i;
                r.fillRectScreen(ax - half, ay - 3 + i, half * 2 + 1, 1, arrow);
            }
        }

        UiRect comboListRect(const UiRect &field, int itemCount, int windowH)
        {
            const int visible = std::clamp(itemCount, 1, kComboMaxVisible);
            const int h = visible * kComboItemH + 4;
            int y = field.bottom() + 2;
            if(y + h > windowH - 8)
                y = std::max(8, field.y - 2 - h);
            return {field.x, y, std::max(field.w, kComboMinListW), h};
        }

        void drawComboList(Renderer &r, BitmapFontRenderer &font, const UiRect &field, const std::vector<std::string> &items, int current, const ComboState &state, int windowH, int mouseX, int mouseY)
        {
            const int n = static_cast<int>(items.size());
            const UiRect list = comboListRect(field, n, windowH);
            r.fillRectScreen(list.x, list.y, list.w, list.h, color::PanelRaised);
            r.drawRectScreen(list.x, list.y, list.w, list.h, color::Accent);
            const int visible = std::min(n, kComboMaxVisible);
            for(int row = 0; row < visible; ++row)
            {
                const int i = state.scroll + row;
                if(i >= n)
                    break;
                const UiRect item {list.x + 2, list.y + 2 + row * kComboItemH, list.w - (n > visible ? 10 : 4), kComboItemH};
                const bool hot = item.contains(mouseX, mouseY);
                if(i == current)
                    r.fillRectScreen(item.x, item.y, item.w, item.h, color::AccentDeep);
                else if(hot)
                    r.fillRectScreen(item.x, item.y, item.w, item.h, color::Hover);
                font.drawString(fitText(font, items[static_cast<size_t>(i)], FONT_BODY, item.w - 20), item.x + 10, item.centerY(), FONT_BODY, i == current ? color::Accent : color::Text, false, Anchor::LEFT | Anchor::VCENTER);
            }
            if(n > visible)
            { // a thin scroll bar for a long list
                const int trackH = list.h - 4;
                const int thumbH = std::max(16, trackH * visible / n);
                const int thumbY = list.y + 2 + (trackH - thumbH) * state.scroll / std::max(1, n - visible);
                r.fillRectScreen(list.right() - 8, list.y + 2, 5, trackH, color::PanelSunken);
                r.fillRectScreen(list.right() - 8, thumbY, 5, thumbH, color::BorderStrong);
            }
        }

        bool comboUpdate(ComboState &state, const UiRect &field, int itemCount, int windowH, int mx, int my, bool clicked, int wheel, int &picked)
        {
            picked = -1;
            if(state.open < 0)
                return false;
            const UiRect list = comboListRect(field, itemCount, windowH);
            const int visible = std::min(itemCount, kComboMaxVisible);
            state.scroll = std::clamp(state.scroll, 0, std::max(0, itemCount - visible));
            if(wheel != 0 && (list.contains(mx, my) || field.contains(mx, my)))
            {
                state.scroll = std::clamp(state.scroll - wheel, 0, std::max(0, itemCount - visible));
                return true;
            }
            if(!clicked)
                return false;
            if(list.contains(mx, my))
            {
                const int row = (my - list.y - 2) / kComboItemH;
                const int index = state.scroll + row;
                if(row >= 0 && row < visible && index >= 0 && index < itemCount)
                    picked = index;
            }
            state.open = -1; // a pick or a click elsewhere closes the list, and the click is not used for anything else
            state.scroll = 0;
            return true;
        }

        void drawLoadingScreen(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const std::string &title, const std::string &status, float progress, Uint32 ticks, SDL_Texture *preview, int previewW, int previewH)
        {
            fillVerticalGradient(r, {0, 0, windowW, windowH}, mix(color::Backdrop, color::PanelRaised, 0.55f), color::Backdrop);
            const int cx = windowW / 2, cy = windowH / 2;

            if(preview && previewW > 0 && previewH > 0)
            {
                // A map is loading: its name on top, its picture in the middle, the bar and the state under it
                font.drawString(fitText(font, title, FONT_H1, windowW - 80), cx, 56, FONT_H1, color::Text, true, Anchor::HCENTER | Anchor::VCENTER);
                r.fillRectScreen(cx - 60, 86, 120, 3, color::Accent);
                const int maxW = std::min(windowW - 120, 640), maxH = std::max(120, windowH - 300);
                const float scale = std::min(static_cast<float>(maxW) / previewW, static_cast<float>(maxH) / previewH);
                const int w = std::max(1, static_cast<int>(previewW * scale)), h = std::max(1, static_cast<int>(previewH * scale));
                const UiRect frame {cx - w / 2 - 6, 110, w + 12, h + 12};
                r.fillRectScreen(frame.x, frame.y, frame.w, frame.h, color::PanelSunken);
                r.drawRectScreen(frame.x, frame.y, frame.w, frame.h, color::BorderStrong);
                const SDL_FRect dst {static_cast<float>(frame.x + 6), static_cast<float>(frame.y + 6), static_cast<float>(w), static_cast<float>(h)};
                SDL_RenderTexture(r.getSDLRenderer(), preview, nullptr, &dst);
                const int barW = std::min(frame.w, 640), barH = 8;
                const UiRect bar {cx - barW / 2, frame.bottom() + 32, barW, barH};
                r.fillRectScreen(bar.x, bar.y, bar.w, bar.h, color::PanelSunken);
                if(progress >= 0.0f)
                {
                    const int fw = static_cast<int>((bar.w - 2) * std::clamp(progress, 0.0f, 1.0f));
                    if(fw > 0)
                        fillHorizontalGradient(r, {bar.x + 1, bar.y + 1, fw, bar.h - 2}, color::AccentDeep, color::Accent);
                }
                r.drawRectScreen(bar.x, bar.y, bar.w, bar.h, color::Border);
                // The turning ring beside the state text
                const int head = static_cast<int>((ticks / 90) % 12);
                const int tx = cx - 90;
                for(int i = 0; i < 12; ++i)
                {
                    const float a = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / 12 - static_cast<float>(M_PI) * 0.5f;
                    const Uint8 alpha = static_cast<Uint8>(255 - ((head - i + 12) % 12) * 19);
                    r.fillRectScreen(tx + static_cast<int>(std::cos(a) * 9.0f) - 1, bar.y + 34 + static_cast<int>(std::sin(a) * 9.0f) - 1, 3, 3, withAlpha(color::Accent, alpha));
                }
                font.drawString(fitText(font, status, FONT_BODY, windowW - 200), cx - 66, bar.y + 34, FONT_BODY, color::TextDim, false, Anchor::LEFT | Anchor::VCENTER);
                return;
            }

            // The name of the game, with an accent line under it
            font.drawString(title, cx, cy - 96, FONT_HERO, color::Text, true, Anchor::HCENTER | Anchor::VCENTER);
            r.fillRectScreen(cx - 60, cy - 58, 120, 3, color::Accent);

            // Turning indicator: a ring of squares, the head bright and a tail fading behind it
            constexpr int kDots = 12;
            const int head = static_cast<int>((ticks / 90) % kDots);
            for(int i = 0; i < kDots; ++i)
            {
                const float a = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / kDots - static_cast<float>(M_PI) * 0.5f;
                const int behind = (head - i + kDots) % kDots;
                const Uint8 alpha = static_cast<Uint8>(255 - behind * 19);
                const int px = cx + static_cast<int>(std::cos(a) * 20.0f) - 2, py = cy - 8 + static_cast<int>(std::sin(a) * 20.0f) - 2;
                r.fillRectScreen(px, py, 5, 5, withAlpha(color::Accent, alpha));
            }

            // Progress bar
            const int barW = std::min(440, windowW * 6 / 10), barH = 8;
            const UiRect bar {cx - barW / 2, cy + 44, barW, barH};
            r.fillRectScreen(bar.x, bar.y, bar.w, bar.h, color::PanelSunken);
            if(progress >= 0.0f)
            {
                const int fw = static_cast<int>((bar.w - 2) * std::clamp(progress, 0.0f, 1.0f));
                if(fw > 0)
                    fillHorizontalGradient(r, {bar.x + 1, bar.y + 1, fw, bar.h - 2}, color::AccentDeep, color::Accent);
            }
            else
            {
                // Unknown length: a piece sliding to and fro
                const int seg = bar.w / 4;
                const float t = static_cast<float>(ticks % 1600) / 800.0f;
                const float pos = t < 1.0f ? t : 2.0f - t;
                const int sx = bar.x + 1 + static_cast<int>((bar.w - 2 - seg) * pos);
                fillHorizontalGradient(r, {sx, bar.y + 1, seg, bar.h - 2}, color::AccentDeep, color::Accent);
            }
            r.drawRectScreen(bar.x, bar.y, bar.w, bar.h, color::Border);

            // What is going on
            font.drawString(fitText(font, status, FONT_BODY, windowW - 80), cx, bar.y + 34, FONT_BODY, color::TextDim, false, Anchor::HCENTER | Anchor::VCENTER);
            if(progress >= 0.0f)
            {
                font.drawString(std::to_string(static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f + 0.5f)) + "%", cx, bar.y - 14, FONT_SMALL_BOLD, color::TextMuted, false, Anchor::HCENTER | Anchor::VCENTER);
            }
        }

        void drawCaption(BitmapFontRenderer &font, Renderer &r, int x, int y, const std::string &text, SDL_Color tick)
        {
            r.fillRectScreen(x, y + 3, 3, 10, tick);
            font.drawString(text, x + 9, y, FONT_SMALL_BOLD, color::TextDim, false);
        }

        std::string fitText(const BitmapFontRenderer &font, const std::string &text, int scale, int maxWidth)
        {
            if(maxWidth <= 0)
                return {};
            if(font.getTextWidth(text, scale) <= maxWidth)
                return text;

            static const std::string kEllipsis = "\xE2\x80\xA6";
            std::string s = text;
            while(!s.empty())
            {
                // Drop one UTF-8 code point from the end
                size_t cut = s.size() - 1;
                while(cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
                    --cut;
                s.erase(cut);
                if(font.getTextWidth(s + kEllipsis, scale) <= maxWidth)
                    return s + kEllipsis;
            }
            return {};
        }

        UiRect centeredRect(int windowW, int windowH, int w, int h, int margin)
        {
            w = std::min(w, windowW - 2 * margin);
            h = std::min(h, windowH - 2 * margin);
            return {(windowW - w) / 2, (windowH - h) / 2, w, h};
        }

    } // namespace ui
} // namespace OpenArtOfWar
