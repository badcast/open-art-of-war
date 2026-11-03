#pragma once

#include "core/Config.hpp"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <functional>
#include <string>

namespace OpenArtOfWar
{

namespace ui
{
struct ConfirmState;
}

enum class UiFont
{
    Small,      // 13px Regular
    Body,       // 16px Regular (Default)
    SmallBold,  // 13px Bold
    BodyBold,   // 16px Bold
    H2,         // 22px Bold
    H1,         // 30px Bold
    Hero        // 48px Bold
};

enum class ButtonStyle
{
    Primary,    // Accent border and highlighted background
    Secondary,  // Standard raised panel with neutral border
    Danger,     // Red border and hazard highlight
    Ghost       // Transparent background with hover highlight
};

class ImGuiBridge
{
public:
    static bool Init(SDL_Window *window, SDL_Renderer *renderer);
    static void Shutdown();

    static void ProcessEvent(const SDL_Event &event);
    static void BeginFrame();
    static void EndFrame();

    static bool WantCaptureMouse();
    static bool WantCaptureKeyboard();

    // Fonts
    static ImFont *GetFont(UiFont font);
    static void PushFont(UiFont font);
    static void PopFont();

    // Theming
    static void ApplyTheme(Faction faction, const SDL_Color *armyColor = nullptr);

    // Widget Helpers
    static bool Button(
        const char *label,
        ButtonStyle style = ButtonStyle::Secondary,
        const ImVec2 &size = ImVec2(0.0f, 0.0f),
        bool enabled = true);

    static void DrawPanel(
        const char *str_id,
        const ImVec2 &size,
        const std::function<void()> &drawContent,
        ImU32 fillCol = 0,
        ImU32 borderCol = 0);

    static void ProgressBar(
        float ratio,
        const ImVec2 &size,
        ImU32 fillColor = 0,
        const char *overlayText = nullptr);

    static bool Checkbox(const char *label, bool *v, bool enabled = true);

    static int DrawConfirmModal(ui::ConfirmState &state, int windowW, int windowH);
};

// RAII helper for pushing and popping a font
struct ScopedFont
{
    explicit ScopedFont(UiFont font)
    {
        ImGuiBridge::PushFont(font);
    }
    ~ScopedFont()
    {
        ImGuiBridge::PopFont();
    }

    ScopedFont(const ScopedFont &) = delete;
    ScopedFont &operator=(const ScopedFont &) = delete;
};

} // namespace OpenArtOfWar
