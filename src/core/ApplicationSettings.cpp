#include "core/Application.hpp"
#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/DebugEnv.hpp"
#include "ui/MenuController.hpp"
#include "ui/HUDController.hpp"
#include "ui/UiKit.hpp"
#include "game/GameplayController.hpp"
#include "game/Sides.hpp"
#include "game/Players.hpp"
#if defined(AOW_DEBUG)
#endif
#include "graphics/TextureManager.hpp"
#include "graphics/Renderer.hpp"
#include "graphics/ShaderEffects.hpp"
#include "graphics/NightLighting.hpp"
#include "net/NetMatch.hpp"
#include "ui/NetworkMenu.hpp"
#include "openaow/net/Discovery.hpp"
#include "openaow/net/MasterClient.hpp"
#include "game/Match.hpp"
#include "game/Simulation.hpp"
#include "graphics/Camera.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "input/InputManager.hpp"
#include "audio/SoundEngine.hpp"
#include "audio/GameSounds.hpp"
#include "audio/SoundRequests.hpp"
#include "game/TileMap.hpp"
#include "game/Pathfinder.hpp"
#include "game/EntityManager.hpp"
#include "game/Economy.hpp"
#include "game/AIController.hpp"
#include "game/MissionController.hpp"
#include "storage/SaveManager.hpp"
#include "debug/DebugOverlay.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cmath>

// Application: applying settings (audio, graphics, window), the "keep these display settings?" dialog and the render
// API switch

namespace OpenArtOfWar
{

void Application::SaveSettings()
{
    m_settings.Save(Settings::DefaultPath());
}

void Application::ApplyAudio()
{
    if (!m_sound)
        return;
    m_sound->SetGeneralVolume(m_settings.generalVolume);
    m_sound->SetMusicVolume(m_settings.musicVolume);
    m_sound->SetFxVolume(m_settings.fxVolume);
    m_sound->SetReverb(m_settings.reverb);
    m_sound->SetVoiceVolume(m_settings.voiceVolume);
    m_sound->SetAmbientVolume(m_settings.ambientVolume);
    m_sound->SetUiVolume(m_settings.uiVolume);
    m_sound->SetVoiceLanguage(m_settings.language); // voices speak the language of the game
    if (m_sound->IsMuted() != m_settings.muted)
        m_sound->ToggleMute();
}

// Graphics quality presets, only about the picture of the game world:
//   low     sprites at 1/3 of the resolution (chunky pixels)
//   medium  sprites at 1/2 of the resolution
//   high    the original resolution
// Smoothing (a separate option) scales the sprites, and the reduced picture back up, smoothly instead of in hard
// pixels.
int Application::RenderDivisor() const
{
    static const int kDivisors[3] = {3, 2, 1};
    return kDivisors[std::clamp(m_settings.graphicsQuality, 0, 2)];
}

void Application::ApplyGraphics()
{
    if (m_renderer)
        m_renderer->SetShowShadows(m_settings.showShadows);
    if (m_camera)
        // The tactical view reaches further out than the player's minimum zoom (tactical_view.min_zoom)
        m_camera->SetZoomLimits(
            std::min(m_settings.zoomMin, m_env.Section("tactical_view").value("min_zoom", 0.1f)), m_settings.zoomMax);
    const bool effects = m_effects && m_effects->Available();
    if (m_effects && m_nightLighting) // the night's light map keeps the picture at gpu_base: the grade gives it back
        m_effects->SetNightExposureScale(1.0f / m_nightLighting->Config().gpuBase);
    if (m_effects)
        m_effects->Configure(
            m_night ? ColorGrade::Night : static_cast<ColorGrade>(m_settings.colorGrade),
            m_settings.glow,
            m_settings.sharpPixels);
    // The pixel-art filter blends texel edges itself and needs linear samples to do it
    if (m_texManager)
        m_texManager->SetSpriteFiltering(m_settings.smoothing, effects && m_settings.sharpPixels);
}

void Application::ApplySettings(SettingsArea area)
{
    m_settings.Clamp();
    switch (area)
    {
        case SettingsArea::Video:
        {
#if OAOW_TOUCH
            m_settings.windowMode = WindowMode::Borderless;
#else
            const Settings before = m_confirmedVideo;
#endif
            VideoModes::Apply(m_window, m_sdlRenderer, m_settings);
            SyncWindowSize();
#if OAOW_TOUCH
            m_confirmedVideo = m_settings;
#else
            const bool windowChanged = before.windowMode != m_settings.windowMode
                                       || before.windowW != m_settings.windowW || before.windowH != m_settings.windowH
                                       || before.fullscreenW != m_settings.fullscreenW
                                       || before.fullscreenH != m_settings.fullscreenH;
            if (windowChanged && !m_videoConfirmActive)
            {
                BeginVideoConfirm(before);
            }
            else if (!windowChanged)
            {
                m_confirmedVideo = m_settings;
            }
#endif
            break;
        }
        case SettingsArea::Graphics:
            ApplyGraphics();
            break;
        case SettingsArea::Audio:
            ApplyAudio();
            break;
        case SettingsArea::Game:
            Localization::SetLanguage(m_settings.language);
            if (m_sound)
                m_sound->SetVoiceLanguage(m_settings.language);
            if (m_aiController)
                m_aiController->SetDifficulty(static_cast<AIDifficulty>(m_settings.difficulty));
            if (m_input)
                m_input->SetEdgePanMargin(m_settings.edgePanMargin);
            HUDController::SetShowBuildGrid(m_settings.showBuildGrid);
            break;
    }
    SaveSettings();
}

// F11 / Alt+Enter: windowed <-> borderless fullscreen
void Application::ToggleFullscreen()
{
#if OAOW_TOUCH
    return; // The platform owns the fullscreen surface.
#endif
    if (m_videoConfirmActive)
        return;
    m_settings.windowMode
        = m_settings.windowMode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed;
    VideoModes::Apply(m_window, m_sdlRenderer, m_settings);
    SyncWindowSize();
    m_confirmedVideo = m_settings;
    SaveSettings();
}

void Application::SyncWindowSize(int w, int h)
{
    if (!m_window)
        return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
    if (pw <= 0 || ph <= 0)
    {
        pw = w;
        ph = h;
    }
    if (pw <= 0 || ph <= 0)
    {
        SDL_GetWindowSize(m_window, &pw, &ph);
    }
    if (pw <= 0 || ph <= 0)
        return;

#if OAOW_TOUCH
    // One shared coordinate space for menus, HUD, camera and touch hit testing: the display scaled uniformly so that
    // it is at least 960x720 units (every dialog fits). A plain render scale, not SDL's logical presentation: clip
    // rects (radar, dialogs, the map edge) misbehaved behind the latter on devices.
    {
        const float scale = std::min(static_cast<float>(ph) / 720.0f, static_cast<float>(pw) / 960.0f);
        SDL_SetRenderLogicalPresentation(m_sdlRenderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        SDL_SetRenderScale(m_sdlRenderer, scale, scale);
        pw = static_cast<int>(std::floor(pw / scale));
        ph = static_cast<int>(std::floor(ph / scale));
    }
#endif
    if (m_windowW == pw && m_windowH == ph)
        return;

    m_windowW = pw;
    m_windowH = ph;
    if (m_camera)
    {
        m_camera->SetViewport(m_windowW - HudReservedWidth(), m_windowH);
        if (m_map)
            m_camera->ClampToBounds(m_map->GetWidth(), m_map->GetHeight());
    }
    if (!m_videoConfirmActive)
    {
        if (m_settings.windowMode == WindowMode::Windowed)
        {
            m_settings.windowW = m_windowW;
            m_settings.windowH = m_windowH;
            m_confirmedVideo.windowW = m_windowW;
            m_confirmedVideo.windowH = m_windowH;
        }
    }
}

void Application::BeginVideoConfirm(const Settings &previous)
{
    m_videoConfirmActive = true;
    m_videoConfirmTimer = 10.0f;
    m_confirmedVideo = previous;
}

namespace
{

struct VideoConfirmLayout
{
    ui::UiRect dialog;
    ui::UiRect keepBtn;
    ui::UiRect revertBtn;
};

VideoConfirmLayout videoConfirmLayout(int windowW, int windowH)
{
    VideoConfirmLayout l;
    l.dialog = ui::CenteredRect(windowW, windowH, 520, 220);
    l.keepBtn = {l.dialog.x + 24, l.dialog.Bottom() - 64, 220, 40};
    l.revertBtn = {l.dialog.Right() - 24 - 220, l.dialog.Bottom() - 64, 220, 40};
    return l;
}

} // namespace

void Application::UpdateVideoConfirm(float deltaTime)
{
    m_videoConfirmTimer -= deltaTime;
    const VideoConfirmLayout l = videoConfirmLayout(m_windowW, m_windowH);
    float lx, ly;
    const bool clicked = m_input->WasLeftClickIssued(lx, ly);
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();

    if (m_renderConfirm)
    {
        if ((clicked && l.keepBtn.Contains(mx, my)) || m_input->IsConfirmJustPressed())
        {
            KeepRenderApi();
        }
        else if (
            (clicked && l.revertBtn.Contains(mx, my)) || m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
            || m_videoConfirmTimer <= 0.0f)
        {
            RevertRenderApi();
        }
        return;
    }

    if ((clicked && l.keepBtn.Contains(mx, my)) || m_input->IsConfirmJustPressed())
    {
        m_videoConfirmActive = false;
        m_confirmedVideo = m_settings;
        SaveSettings();
    }
    else if (
        (clicked && l.revertBtn.Contains(mx, my)) || m_input->IsKeyJustPressed(SDL_SCANCODE_ESCAPE)
        || m_videoConfirmTimer <= 0.0f)
    {
        m_videoConfirmActive = false;
        // Back to the previous window setup, the other video options stay as chosen
        m_settings.windowMode = m_confirmedVideo.windowMode;
        m_settings.windowW = m_confirmedVideo.windowW;
        m_settings.windowH = m_confirmedVideo.windowH;
        m_settings.fullscreenW = m_confirmedVideo.fullscreenW;
        m_settings.fullscreenH = m_confirmedVideo.fullscreenH;
        VideoModes::Apply(m_window, m_sdlRenderer, m_settings);
        SyncWindowSize();
        m_confirmedVideo = m_settings;
        SaveSettings();
    }
}

void Application::RenderVideoConfirm()
{
    const VideoConfirmLayout l = videoConfirmLayout(m_windowW, m_windowH);
    m_renderer->FillRectScreen(0, 0, m_windowW, m_windowH, ui::color::Scrim);
    ui::UiRect content = ui::DrawDialog(
        *m_renderer,
        *m_fontRenderer,
        m_windowW,
        m_windowH,
        l.dialog,
        m_renderConfirm ? _("KEEP THIS RENDER API?") : _("KEEP THESE DISPLAY SETTINGS?"));
    const int seconds = std::max(0, static_cast<int>(std::ceil(m_videoConfirmTimer)));
    m_fontRenderer->DrawString(
        _("The previous settings come back automatically in") + std::string(" ") + std::to_string(seconds) + " "
            + _("s"),
        content.CenterX(),
        content.y + 24,
        ui::FONT_BODY,
        ui::color::TextDim,
        false,
        Anchor::HCENTER | Anchor::VCENTER);
    const int mx = m_input->GetMouseScreenX(), my = m_input->GetMouseScreenY();
    ui::DrawButton(
        *m_renderer, *m_fontRenderer, l.keepBtn, _("KEEP"), ui::ButtonStyle::Primary, l.keepBtn.Contains(mx, my));
    ui::DrawButton(
        *m_renderer,
        *m_fontRenderer,
        l.revertBtn,
        _("REVERT"),
        ui::ButtonStyle::Secondary,
        l.revertBtn.Contains(mx, my));
}

void Application::RequestRestart()
{
    m_restartRequested = true;
    m_running = false;
}

void Application::SwitchRenderApi()
{
    const char *active = SDL_GetRendererName(m_sdlRenderer);
    if (!active || RenderBackend::Configured(m_env) == active)
        return;
    m_env.Edit("render")["confirm_from"] = active;
    m_env.Save();
    RequestRestart();
}

void Application::KeepRenderApi()
{
    m_renderConfirm = false;
    m_videoConfirmActive = false;
    m_env.Edit("render").erase("confirm_from");
    m_env.Save();
}

void Application::RevertRenderApi()
{
    const std::string previous = m_env.Section("render").value("confirm_from", std::string());
    m_renderConfirm = false;
    m_videoConfirmActive = false;
    m_env.Edit("render").erase("confirm_from");
    RenderBackend::select(m_env, previous); // saves
    if (!previous.empty())
        RequestRestart();
}

} // namespace OpenArtOfWar
