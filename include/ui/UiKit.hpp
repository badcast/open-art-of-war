#pragma once

#include "core/Config.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

    class Renderer;
    class BitmapFontRenderer;

    /**
     * @brief Shared look & feel for menus, dialogs and the in-game sidebar.
     *
     * Every screen builds its geometry from plain UiRect values so that the same rectangles
     * drive both rendering and mouse hit-testing.
     */
    namespace ui
    {

        // Font scales understood by BitmapFontRenderer
        constexpr int FONT_SMALL = 1;      // 13px regular
        constexpr int FONT_BODY = 2;       // 16px regular
        constexpr int FONT_H2 = 3;         // 22px bold
        constexpr int FONT_H1 = 4;         // 30px bold
        constexpr int FONT_SMALL_BOLD = 5; // 13px bold
        constexpr int FONT_BODY_BOLD = 6;  // 16px bold
        constexpr int FONT_HERO = 7;       // 48px bold

        struct UiRect
        {
            int x {0};
            int y {0};
            int w {0};
            int h {0};

            bool contains(int px, int py) const
            {
                return px >= x && px < x + w && py >= y && py < y + h;
            }
            int right() const
            {
                return x + w;
            }
            int bottom() const
            {
                return y + h;
            }
            int centerX() const
            {
                return x + w / 2;
            }
            int centerY() const
            {
                return y + h / 2;
            }
            UiRect inset(int d) const
            {
                return {x + d, y + d, w - 2 * d, h - 2 * d};
            }
        };

        // Palette. Panels, borders and the accent follow the theme (neutral in menus, the player's faction in game,
        // see applyTheme); text and status colors are fixed.
        namespace color
        {
            inline SDL_Color Backdrop {8, 10, 14, 255};
            inline SDL_Color Scrim {4, 6, 9, 190};
            inline SDL_Color Panel {17, 21, 28, 246};
            inline SDL_Color PanelRaised {25, 30, 40, 255};
            inline SDL_Color PanelSunken {11, 14, 19, 255};
            inline SDL_Color Border {50, 60, 76, 255};
            inline SDL_Color BorderStrong {78, 92, 114, 255};
            inline SDL_Color Hover {33, 40, 53, 255};
            inline SDL_Color Text {226, 231, 238, 255};
            inline SDL_Color TextDim {150, 160, 176, 255};
            inline SDL_Color TextMuted {100, 110, 126, 255};
            inline SDL_Color Accent {232, 176, 74, 255};
            inline SDL_Color AccentDeep {120, 84, 30, 255};
            inline SDL_Color AccentText {24, 18, 8, 255};
            inline SDL_Color Danger {226, 88, 74, 255};
            inline SDL_Color DangerDeep {70, 26, 22, 255};
            inline SDL_Color Ok {104, 204, 124, 255};
            inline SDL_Color Info {96, 174, 232, 255};
        } // namespace color

        // Game UI colors: Faction::Resistance (rust red), Faction::Confederation (steel blue), Faction::Neutral (menus)
        // `army`: the player's own army color (the game screen wears it as its accent); nullptr keeps the faction look
        void applyTheme(Faction faction, const SDL_Color *army = nullptr);
        // Reads assets/characters/game_ui.yaml (themes per faction + the fixed colors) over the built-in look; false if it
        // cannot be parsed. Call before applyTheme.
        bool loadTheme(const std::string &yamlText);

        SDL_Color withAlpha(SDL_Color c, Uint8 a);
        SDL_Color mix(SDL_Color a, SDL_Color b, float t);

        void fillVerticalGradient(Renderer &r, const UiRect &rect, SDL_Color top, SDL_Color bottom);
        void fillHorizontalGradient(Renderer &r, const UiRect &rect, SDL_Color left, SDL_Color right);

        // Flat plate with a hairline border and a soft top highlight
        void drawPanel(Renderer &r, const UiRect &rect, SDL_Color fill = color::Panel, SDL_Color border = color::Border);

        // Full-screen dimmer + framed dialog with a title bar; returns the content area below the title
        UiRect drawDialog(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const UiRect &rect, const std::string &title);

        enum class ButtonStyle
        {
            Primary,
            Secondary,
            Danger,
            Ghost
        };
        void drawButton(Renderer &r, BitmapFontRenderer &font, const UiRect &rect, const std::string &label, ButtonStyle style, bool hovered, bool enabled = true);

        void drawProgressBar(Renderer &r, const UiRect &rect, float ratio, SDL_Color fillColor);

        // A yes / no question over a dimmed screen (leaving a room or a game). Enter / click Yes confirms, Esc / click No cancels.
        struct ConfirmState
        {
            bool open {false};
            std::string title, text;
            int action {0}; // what the owner asked about (his own numbering)
        };
        // Returns 1 for Yes, 0 for No, -1 while the question stays open
        int confirmUpdate(const ConfirmState &state, int windowW, int windowH, int mx, int my, bool clicked, bool enterPressed, bool escPressed);
        void drawConfirm(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const ConfirmState &state, int mx, int my);

        // Combo box: a field with the current value and an arrow; opened, a list of the choices drops under it (over
        // everything else, so draw it last). One combo of a screen is open at a time: ComboState says which.
        struct ComboState
        {
            int open {-1};  // the id the owner gave to the open combo, -1 none
            int scroll {0}; // first visible item of a long list
        };
        constexpr int kComboItemH = 28;
        constexpr int kComboMaxVisible = 9;
        constexpr int kComboMinListW = 150; // a narrow field (a table cell) still gets a readable list

        void drawComboField(Renderer &r, BitmapFontRenderer &font, const UiRect &rect, const std::string &value, bool open, bool hovered, bool enabled = true, SDL_Color valueColor = color::Text);
        // Where the list of `itemCount` choices of the combo at `field` goes (under it, above when there is no room)
        UiRect comboListRect(const UiRect &field, int itemCount, int windowH);
        void drawComboList(Renderer &r, BitmapFontRenderer &font, const UiRect &field, const std::vector<std::string> &items, int current, const ComboState &state, int windowH, int mouseX, int mouseY);
        // Input for the open combo (call it before anything else reacts to the click): a click on a choice sets `picked`, a click
        // elsewhere closes the list; the mouse wheel scrolls it. Returns true when the input belongs to the combo.
        bool comboUpdate(ComboState &state, const UiRect &field, int itemCount, int windowH, int mx, int my, bool clicked, int wheel, int &picked);

        // Full-screen loading screen: the game name, what is being done (`status`), a progress bar (`progress` 0..1; a negative
        // value shows a sliding bar for something of unknown length) and a turning indicator. `ticks` is SDL_GetTicks().
        // With `preview` (a map's picture, previewW x previewH) the picture takes the middle of the screen.
        void drawLoadingScreen(Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const std::string &title, const std::string &status, float progress, Uint32 ticks, SDL_Texture *preview = nullptr, int previewW = 0, int previewH = 0);

        // Small all-caps section caption with an accent tick on the left
        void drawCaption(BitmapFontRenderer &font, Renderer &r, int x, int y, const std::string &text, SDL_Color tick = color::Accent);

        // Shortens text with an ellipsis so it fits maxWidth pixels
        std::string fitText(const BitmapFontRenderer &font, const std::string &text, int scale, int maxWidth);

        // Centered dialog rectangle clamped to the window with a margin
        UiRect centeredRect(int windowW, int windowH, int w, int h, int margin = 24);

    } // namespace ui
} // namespace OpenArtOfWar
