#include "ui/ImGuiBridge.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Localization.hpp"
#include "ui/UiKit.hpp"
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <vector>

namespace OpenArtOfWar
{

namespace
{

SDL_Window *s_window = nullptr;
SDL_Renderer *s_renderer = nullptr;
bool s_initialized = false;
bool s_frameActive = false;

std::vector<std::uint8_t> s_fontDataRegular;
std::vector<std::uint8_t> s_fontDataBold;
ImFont *s_fonts[7] = {nullptr};

ImVec4 ToVec4(SDL_Color c)
{
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

[[maybe_unused]] ImU32 ToU32(SDL_Color c)
{
    return IM_COL32(c.r, c.g, c.b, c.a);
}

} // namespace

bool ImGuiBridge::Init(SDL_Window *window, SDL_Renderer *renderer)
{
    if (s_initialized)
        return true;
    if (!window || !renderer)
        return false;

    s_window = window;
    s_renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load fonts with full Cyrillic support
    const std::string regPath = LoaderResources::FontRegular();
    const std::string boldPath = LoaderResources::FontBold();

    const bool hasRegular = Assets::ReadBytes(regPath, s_fontDataRegular);
    const bool hasBold = Assets::ReadBytes(boldPath, s_fontDataBold);

    if (hasRegular && hasBold)
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;

        const ImWchar *ranges = io.Fonts->GetGlyphRangesCyrillic();

        // 0: Small (13px Regular)
        s_fonts[0] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataRegular.data(), static_cast<int>(s_fontDataRegular.size()), 13.0f, &cfg, ranges);
        // 1: Body (16px Regular, Default)
        s_fonts[1] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataRegular.data(), static_cast<int>(s_fontDataRegular.size()), 16.0f, &cfg, ranges);
        // 2: SmallBold (13px Bold)
        s_fonts[2] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataBold.data(), static_cast<int>(s_fontDataBold.size()), 13.0f, &cfg, ranges);
        // 3: BodyBold (16px Bold)
        s_fonts[3] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataBold.data(), static_cast<int>(s_fontDataBold.size()), 16.0f, &cfg, ranges);
        // 4: H2 (22px Bold)
        s_fonts[4] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataBold.data(), static_cast<int>(s_fontDataBold.size()), 22.0f, &cfg, ranges);
        // 5: H1 (30px Bold)
        s_fonts[5] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataBold.data(), static_cast<int>(s_fontDataBold.size()), 30.0f, &cfg, ranges);
        // 6: Hero (48px Bold)
        s_fonts[6] = io.Fonts->AddFontFromMemoryTTF(
            s_fontDataBold.data(), static_cast<int>(s_fontDataBold.size()), 48.0f, &cfg, ranges);

        io.FontDefault = s_fonts[1];
        io.Fonts->Build();
    }
    else
    {
        std::cerr << "[ImGuiBridge] Warning: could not read font files from assets: " << regPath << " / " << boldPath
                  << ", falling back to default font." << std::endl;
        s_fonts[0] = io.Fonts->AddFontDefault();
        s_fonts[1] = s_fonts[0];
        s_fonts[2] = s_fonts[0];
        s_fonts[3] = s_fonts[0];
        s_fonts[4] = s_fonts[0];
        s_fonts[5] = s_fonts[0];
        s_fonts[6] = s_fonts[0];
    }

    // Apply baseline military RTS styling
    ApplyTheme(Faction::Neutral, nullptr);

    ImGui_ImplSDL3_InitForSDLRenderer(s_window, s_renderer);
    ImGui_ImplSDLRenderer3_Init(s_renderer);

    s_initialized = true;
    return true;
}

void ImGuiBridge::Shutdown()
{
    if (!s_initialized)
        return;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    s_fontDataRegular.clear();
    s_fontDataBold.clear();
    for (int i = 0; i < 7; ++i)
        s_fonts[i] = nullptr;

    s_window = nullptr;
    s_renderer = nullptr;
    s_initialized = false;
    s_frameActive = false;
}

void ImGuiBridge::ProcessEvent(const SDL_Event &event)
{
    if (s_initialized)
    {
        // With a logical presentation (Android) pointer events must land in the logical space the UI is laid out in
        SDL_Event converted = event;
        SDL_ConvertEventToRenderCoordinates(s_renderer, &converted);
        ImGui_ImplSDL3_ProcessEvent(&converted);
    }
}

void ImGuiBridge::BeginFrame()
{
    if (!s_initialized || s_frameActive)
        return;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    {
        // The window is drawn through a logical presentation on Android: ImGui must use that space, not the window's
        int lw = 0, lh = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        if (SDL_GetRenderLogicalPresentation(s_renderer, &lw, &lh, &mode) && mode != SDL_LOGICAL_PRESENTATION_DISABLED
            && lw > 0 && lh > 0)
        {
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(lw), static_cast<float>(lh));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
    }
    ImGui::NewFrame();
    s_frameActive = true;
}

void ImGuiBridge::EndFrame()
{
    if (!s_initialized || !s_frameActive)
        return;

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);
    s_frameActive = false;
}

bool ImGuiBridge::IsInitialized()
{
    return s_initialized;
}

bool ImGuiBridge::IsFrameActive()
{
    return s_initialized && s_frameActive;
}

bool ImGuiBridge::WantCaptureMouse()
{
    if (!s_initialized)
        return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiBridge::WantCaptureKeyboard()
{
    if (!s_initialized)
        return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

ImFont *ImGuiBridge::GetFont(UiFont font)
{
    if (!s_initialized)
        return nullptr;
    const int idx = static_cast<int>(font);
    if (idx >= 0 && idx < 7 && s_fonts[idx])
        return s_fonts[idx];
    return ImGui::GetFont();
}

void ImGuiBridge::PushFont(UiFont font)
{
    if (!s_initialized || !s_frameActive)
        return;
    ImGui::PushFont(GetFont(font));
}

void ImGuiBridge::PopFont()
{
    if (!s_initialized || !s_frameActive)
        return;
    ImGui::PopFont();
}

void ImGuiBridge::ApplyTheme(Faction faction, const SDL_Color *armyColor)
{
    // Sync the underlying ui::color palette first
    ui::ApplyTheme(faction, armyColor);
    if (!ImGui::GetCurrentContext())
        return;

    ImGuiStyle &style = ImGui::GetStyle();

    // Geometric metrics: sharp military panels with 1px border
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 10.0f;

    // Palette mapping to ui::color
    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ToVec4(ui::color::Panel);
    colors[ImGuiCol_ChildBg] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_PopupBg] = ToVec4(ui::color::Panel);
    colors[ImGuiCol_Border] = ToVec4(ui::color::Border);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_FrameBgHovered] = ToVec4(ui::color::Hover);
    colors[ImGuiCol_FrameBgActive] = ToVec4(ui::color::PanelRaised);
    colors[ImGuiCol_TitleBg] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_TitleBgActive] = ToVec4(ui::color::Panel);
    colors[ImGuiCol_TitleBgCollapsed] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_ScrollbarBg] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_ScrollbarGrab] = ToVec4(ui::color::Border);
    colors[ImGuiCol_ScrollbarGrabHovered] = ToVec4(ui::color::BorderStrong);
    colors[ImGuiCol_ScrollbarGrabActive] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_CheckMark] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_SliderGrab] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_SliderGrabActive] = ToVec4(ui::color::AccentDeep);
    colors[ImGuiCol_Button] = ToVec4(ui::color::PanelRaised);
    colors[ImGuiCol_ButtonHovered] = ToVec4(ui::color::Hover);
    colors[ImGuiCol_ButtonActive] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_Header] = ToVec4(ui::color::Hover);
    colors[ImGuiCol_HeaderHovered] = ToVec4(ui::color::PanelRaised);
    colors[ImGuiCol_HeaderActive] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_Separator] = ToVec4(ui::color::Border);
    colors[ImGuiCol_SeparatorHovered] = ToVec4(ui::color::BorderStrong);
    colors[ImGuiCol_SeparatorActive] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_ResizeGrip] = ToVec4(ui::color::Border);
    colors[ImGuiCol_ResizeGripHovered] = ToVec4(ui::color::BorderStrong);
    colors[ImGuiCol_ResizeGripActive] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_TabHovered] = ToVec4(ui::color::Hover);
    colors[ImGuiCol_Tab] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_TabSelected] = ToVec4(ui::color::PanelRaised);
    colors[ImGuiCol_TabSelectedOverline] = ToVec4(ui::color::Accent);
    colors[ImGuiCol_TabDimmed] = ToVec4(ui::color::PanelSunken);
    colors[ImGuiCol_TabDimmedSelected] = ToVec4(ui::color::Panel);
    colors[ImGuiCol_Text] = ToVec4(ui::color::Text);
    colors[ImGuiCol_TextDisabled] = ToVec4(ui::color::TextMuted);
    colors[ImGuiCol_ModalWindowDimBg] = ToVec4(ui::color::Scrim);
}

bool ImGuiBridge::Button(const char *label, ButtonStyle style, const ImVec2 &size, bool enabled)
{
    if (!s_initialized || !s_frameActive)
        return false;
    if (!enabled)
        ImGui::BeginDisabled(true);

    int pushedColors = 0;
    if (style == ButtonStyle::Primary)
    {
        ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(ui::color::Accent));
        ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(ui::color::PanelRaised));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(ui::color::Hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(ui::color::Accent));
        pushedColors += 4;
    }
    else if (style == ButtonStyle::Danger)
    {
        ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(ui::color::Danger));
        ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(ui::color::DangerDeep));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(ui::color::Danger));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(ui::color::Accent));
        pushedColors += 4;
    }
    else if (style == ButtonStyle::Ghost)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToVec4(ui::color::Hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToVec4(ui::color::PanelRaised));
        pushedColors += 4;
    }

    const bool clicked = ImGui::Button(label, size);

    if (pushedColors > 0)
        ImGui::PopStyleColor(pushedColors);

    if (!enabled)
        ImGui::EndDisabled();

    return clicked && enabled;
}

void ImGuiBridge::DrawPanel(
    const char *str_id, const ImVec2 &size, const std::function<void()> &drawContent, ImU32 fillCol, ImU32 borderCol)
{
    if (!s_initialized || !s_frameActive)
        return;
    int pushedColors = 0;
    if (fillCol != 0)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, fillCol);
        pushedColors++;
    }
    if (borderCol != 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
        pushedColors++;
    }

    if (ImGui::BeginChild(str_id, size, ImGuiChildFlags_Borders))
    {
        if (drawContent)
            drawContent();
    }
    ImGui::EndChild();

    if (pushedColors > 0)
        ImGui::PopStyleColor(pushedColors);
}

void ImGuiBridge::ProgressBar(float ratio, const ImVec2 &size, ImU32 fillColor, const char *overlayText)
{
    if (!s_initialized || !s_frameActive)
        return;
    const int pushedColors = fillColor != 0 ? 1 : 0;
    if (fillColor != 0)
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fillColor);

    ImGui::ProgressBar(ratio, size, overlayText);

    if (pushedColors > 0)
        ImGui::PopStyleColor(pushedColors);
}

bool ImGuiBridge::Checkbox(const char *label, bool *v, bool enabled)
{
    if (!s_initialized || !s_frameActive)
        return false;
    if (!enabled)
        ImGui::BeginDisabled(true);

    const bool changed = ImGui::Checkbox(label, v);

    if (!enabled)
        ImGui::EndDisabled();

    return changed && enabled;
}

int ImGuiBridge::DrawConfirmModal(ui::ConfirmState &state, int windowW, int windowH)
{
    if (!state.open || state.modalResult >= 0 || !s_initialized || !s_frameActive)
        return state.modalResult;

    int result = -1;
    const std::string popupId = state.title.empty() ? "Confirm" : state.title;

    if (!ImGui::IsPopupOpen(popupId.c_str()))
        ImGui::OpenPopup(popupId.c_str());

    ImGui::SetNextWindowPos(ImVec2(windowW * 0.5f, windowH * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
    if (ImGui::BeginPopupModal(popupId.c_str(), nullptr, flags))
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorStartPos().x + 430.0f);
        ImGui::TextUnformatted(state.text.c_str());
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonW = 120.0f;
        const float spacing = 20.0f;
        const float totalW = buttonW * 2 + spacing;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalW) * 0.5f);

        if (Button(_("YES"), ButtonStyle::Danger, ImVec2(buttonW, 32.0f)))
        {
            result = 1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0.0f, spacing);

        if (Button(_("NO"), ButtonStyle::Secondary, ImVec2(buttonW, 32.0f)))
        {
            result = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (result >= 0)
        state.modalResult = result;
    return result;
}

} // namespace OpenArtOfWar
