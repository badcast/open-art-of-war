#pragma once

#include "core/Config.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <string_view>
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

    bool Contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    int Right() const
    {
        return x + w;
    }
    int Bottom() const
    {
        return y + h;
    }
    int CenterX() const
    {
        return x + w / 2;
    }
    int CenterY() const
    {
        return y + h / 2;
    }
    UiRect Inset(int d) const
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
inline SDL_Color Panel {23, 19, 16, 246};
inline SDL_Color PanelRaised {35, 28, 23, 255};
inline SDL_Color PanelSunken {16, 13, 10, 255};
inline SDL_Color Border {90, 66, 48, 255};
inline SDL_Color BorderStrong {140, 102, 66, 255};
inline SDL_Color Hover {51, 40, 31, 255};
inline SDL_Color Text {237, 230, 220, 255};
inline SDL_Color TextDim {180, 168, 152, 255};
inline SDL_Color TextMuted {125, 114, 101, 255};
inline SDL_Color Accent {232, 176, 74, 255};
inline SDL_Color AccentDeep {120, 84, 30, 255};
inline SDL_Color AccentText {24, 18, 8, 255};
inline SDL_Color Danger {226, 88, 74, 255};
inline SDL_Color DangerDeep {70, 26, 22, 255};
inline SDL_Color Ok {104, 204, 124, 255};
inline SDL_Color Info {96, 174, 232, 255};
inline SDL_Color White {255, 255, 255, 255};
} // namespace color

// Game UI colors: Faction::Resistance (rust red), Faction::Confederation (steel blue), Faction::Neutral (menus)
// `army`: the player's own army color (the game screen wears it as its accent); nullptr keeps the faction look
void ApplyTheme(Faction faction, const SDL_Color *army = nullptr);
// Reads assets/characters/game_ui.yaml (themes per faction + the fixed colors) over the built-in look; false if it
// cannot be parsed. Call before applyTheme.
bool LoadTheme(std::string_view yamlText);

inline SDL_Color WithAlpha(SDL_Color c, Uint8 a)
{
    c.a = a;
    return c;
}

SDL_Color Mix(SDL_Color a, SDL_Color b, float t);

void FillVerticalGradient(Renderer &r, const UiRect &rect, SDL_Color top, SDL_Color bottom);
void FillHorizontalGradient(Renderer &r, const UiRect &rect, SDL_Color left, SDL_Color right);

// Flat "slate" skin of the in-game HUD, the look of the touch interface: flat dark plates, a pale rim and the army
// colour for buttons, tabs and the active bar. Panels, tabs and buttons drawn while it is on use it.
void SetFlatSkin(bool on);
bool FlatSkin();
struct FlatSkinScope
{
    bool previous;
    FlatSkinScope()
        : previous(FlatSkin())
    {
        SetFlatSkin(true);
    }
    ~FlatSkinScope()
    {
        SetFlatSkin(previous);
    }
    FlatSkinScope(const FlatSkinScope &) = delete;
    FlatSkinScope &operator=(const FlatSkinScope &) = delete;
};

// Panel chrome backed by sprite_dialog_items borders/corners/fill (with flat fallback if sheets aren't loaded)
void DrawPanel(Renderer &r, const UiRect &rect, SDL_Color fill = color::Panel, SDL_Color border = color::Border);

// Panel border/corner frame overlay without altering interior contents
void DrawPanelFrame(Renderer &r, const UiRect &rect, SDL_Color border = color::BorderStrong);

// Sharp rectangular tactical tab plate with bottom active indicator bar
void DrawTab(
    Renderer &r,
    const UiRect &rect,
    bool active,
    bool hovered,
    SDL_Color accent = color::Accent,
    BitmapFontRenderer *font = nullptr,
    std::string_view label = {});

// Screen origin and proportional scale placing a [minX..maxX, minY..maxY] composite dead center inside `box`.
struct CompositeFit
{
    int x {0};
    int y {0};
    float scale {1.0f};
};
CompositeFit FitCompositeInRect(const UiRect &box, int pad, float minX, float minY, float maxX, float maxY);

// Draws a composite proportionally scaled to fit inside `box` (with `pad` margin) and centered within it.
void DrawCompositeFitted(Renderer &r, int compId, const UiRect &box, int pad, Faction faction = Faction::Resistance);

// Full-screen dimmer + framed dialog with a title bar; returns the content area below the title
UiRect DrawDialog(
    Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const UiRect &rect, std::string_view title);

enum class ButtonStyle
{
    Primary,
    Secondary,
    Danger,
    Ghost
};
void DrawButton(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view label,
    ButtonStyle style,
    bool hovered,
    bool enabled = true,
    int fontId = FONT_BODY_BOLD,
    Uint8 opacity = 255);

// Tactical beveled plate with optional scanline texture and left indicator bar
void DrawButtonChrome(
    Renderer &r,
    const UiRect &rect,
    SDL_Color topFill,
    SDL_Color bottomFill,
    SDL_Color border,
    SDL_Color accentBar,
    SDL_Color cornerColor,
    Uint8 opacity = 255,
    bool horizontalGradient = false);

// The bold font nearest to `scale` times the body size (16px): 1 = body, 2 = ~30px title, 3 = hero (48px)
int ButtonFontForScale(float scale);

void DrawCheckbox(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view label,
    bool checked,
    bool hovered,
    bool enabled = true);

void DrawProgressBar(Renderer &r, const UiRect &rect, float ratio, SDL_Color fillColor);
// A thin scroll bar along `track` (vertical when it is taller than wide): `pos` of `total - visible`, the thumb as
// long as the visible share. Nothing is drawn when everything fits.
UiRect ScrollThumb(const UiRect &track, float pos, float total, float visible);
void DrawScrollBar(Renderer &r, const UiRect &track, float pos, float total, float visible, bool hot = false);
// The scroll position that puts the middle of the thumb under the mouse (`mouse`: x or y along the track)
float ScrollPosFromMouse(const UiRect &track, int mouse, float total, float visible);
// Scroll bars are thin: they are grabbed from a wider strip
bool OnScrollTrack(const UiRect &track, int mx, int my);

// Shared 1D/2D kinetic scroll solver (wheel easing, pointer drag past 12 px slop, and flick glide)
struct KineticScroll2D
{
    float offsetX {0.0f}, offsetY {0.0f};
    float targetX {0.0f}, targetY {0.0f};
    float velX {0.0f}, velY {0.0f};
    bool dragging {false};
    bool dragMoved {false};
    bool hasVelocity {false};
    float dragStartX {0.0f}, dragStartY {0.0f};
    float dragLastX {0.0f}, dragLastY {0.0f};
    float dragIdleS {0.0f};
    Uint64 lastTickNs {0};

    void Reset()
    {
        offsetX = offsetY = targetX = targetY = velX = velY = 0.0f;
        dragging = dragMoved = hasVelocity = false;
        dragStartX = dragStartY = dragLastX = dragLastY = 0.0f;
        dragIdleS = 0.0f;
        lastTickNs = 0;
    }
    void StopGlide()
    {
        velX = velY = 0.0f;
        hasVelocity = false;
    }
};

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
    bool allowStart = true,
    bool allowWheelOutside = false,
    const UiRect *extraWheelRect = nullptr);

// Smooth kinetic list scrolling: smooth mouse-wheel easing, pointer drag past 12 px slop, and flick glide
struct SmoothListScroll
{
    float offset {0.0f};
    float target {0.0f};
    float velocity {0.0f};
    bool dragging {false};
    bool dragMoved {false};
    bool hasVelocity {false};
    float dragStartY {0.0f};
    float dragLastY {0.0f};
    float dragIdleS {0.0f};
    bool barDrag {false};
    Uint64 lastTickNs {0};

    void Reset()
    {
        offset = target = velocity = 0.0f;
        dragging = dragMoved = hasVelocity = barDrag = false;
        dragStartY = dragLastY = dragIdleS = 0.0f;
        lastTickNs = 0;
    }
    void StopGlide()
    {
        velocity = 0.0f;
        hasVelocity = false;
    }
    void EnsureVisible(int row, int stepH, int viewH, int totalRows);
    int FirstVisibleRow(int stepH) const
    {
        return stepH > 0 ? (offset > 0.0f ? static_cast<int>(offset) / stepH : 0) : 0;
    }
    int PixelOffset() const
    {
        return static_cast<int>(offset + 0.5f);
    }
};
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
    bool allowBarDrag = true);

// A yes / no question over a dimmed screen (leaving a room or a game). Enter / click Yes confirms, Esc / click No
// cancels.
struct ConfirmState
{
    bool open {false};
    std::string title, text;
    int action {0}; // what the owner asked about (his own numbering)
    mutable int modalResult {-1};
};
// Returns 1 for Yes, 0 for No, -1 while the question stays open
int ConfirmUpdate(
    const ConfirmState &state,
    int windowW,
    int windowH,
    int mx,
    int my,
    bool clicked,
    bool enterPressed,
    bool escPressed);
void DrawConfirm(
    Renderer &r, BitmapFontRenderer &font, int windowW, int windowH, const ConfirmState &state, int mx, int my);

// Combo box: a field with the current value and an arrow; opened, a list of the choices drops under it (over
// everything else, so draw it last). One combo of a screen is open at a time: ComboState says which.
struct ComboState
{
    int open {-1};     // the id the owner gave to the open combo, -1 none
    int scroll {0};    // first visible item of a long list
    int lastOpen {-1}; // detects newly opened combos to sync smooth pixel offset
    float scrollPx {0.0f};
    float targetPx {0.0f};
    float velPx {0.0f};
    bool dragging {false};
    bool dragMoved {false};
    bool hasVelocity {false};
    float dragStartY {0.0f};
    float dragLastY {0.0f};
    float dragIdleS {0.0f};
    Uint64 lastTickNs {0};
};
constexpr int kComboItemH = 34;
constexpr int kComboMaxVisible = 9;
constexpr int kComboMinListW = 190; // a narrow field (a table cell) still gets a readable list

void DrawComboField(
    Renderer &r,
    BitmapFontRenderer &font,
    const UiRect &rect,
    std::string_view value,
    bool open,
    bool hovered,
    bool enabled = true,
    SDL_Color valueColor = color::Text,
    int fontId = FONT_BODY);
// Where the list of `itemCount` choices of the combo at `field` goes (under it, above when there is no room)
UiRect ComboListRect(const UiRect &field, int itemCount, int windowH);
// `disabled` (optional, one flag per item): choices shown dimmed that cannot be picked (the owner ignores them)
// `swatches` (optional, one colour per item): a colour square drawn to the left of each choice's label
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
    const std::vector<bool> *disabled = nullptr,
    const std::vector<SDL_Color> *swatches = nullptr);
// Input for the open combo (call it before anything else reacts to the click): a click on a choice sets `picked`, a
// click elsewhere closes the list; the mouse wheel (and optional drag) scrolls it.
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
    bool down = false,
    bool pressed = false,
    int pressX = 0,
    int pressY = 0);

// Full-screen loading screen: the game name, what is being done (`status`), a progress bar (`progress` 0..1; a negative
// value shows a sliding bar for something of unknown length) and a turning indicator. `ticks` is SDL_GetTicks().
// With `preview` (a map's picture, previewW x previewH) the picture takes the middle of the screen.
void DrawLoadingScreen(
    Renderer &r,
    BitmapFontRenderer &font,
    int windowW,
    int windowH,
    std::string_view title,
    std::string_view status,
    float progress,
    Uint32 ticks,
    SDL_Texture *preview = nullptr,
    int previewW = 0,
    int previewH = 0);

// Small all-caps section caption with an accent tick on the left
void DrawCaption(
    BitmapFontRenderer &font, Renderer &r, int x, int y, std::string_view text, SDL_Color tick = color::Accent);

// Shortens text with an ellipsis so it fits maxWidth pixels
std::string FitText(const BitmapFontRenderer &font, std::string_view text, int scale, int maxWidth);

// Centered dialog rectangle clamped to the window with a margin
UiRect CenteredRect(int windowW, int windowH, int w, int h, int margin = 24);

} // namespace ui
} // namespace OpenArtOfWar
