#pragma once

#include <string>

namespace OpenArtOfWar
{

    enum class WindowMode : int
    {
        Windowed = 0,
        Borderless = 1, // fullscreen at the desktop resolution
        Exclusive = 2   // fullscreen at the chosen display mode
    };

    // Which group of options just changed (what the game has to apply)
    enum class SettingsArea
    {
        Video,
        Graphics,
        Audio,
        Game
    };

    // Options of the player, stored in settings.json in the per-user directory (Assets::userDataDir())
    struct Settings
    {
        // Video
        WindowMode windowMode {WindowMode::Windowed};
        int windowW {1280};
        int windowH {720};
        int fullscreenW {1920}; // display mode of the exclusive fullscreen
        int fullscreenH {1080};
        bool vsync {true};
        int fpsLimit {0}; // 0 = unlimited (frames per second), only used without vsync or below the refresh rate
        bool showFps {false};

        // Graphics
        int graphicsQuality {2};  // 0 = low (sprites at 1/3 resolution), 1 = medium (1/2), 2 = high (original resolution)
        bool smoothing {true};    // smooth scaling of the sprites (and of the reduced picture of low / medium quality)
        float zoomDefault {2.0f}; // camera zoom when a mission starts
        float zoomMin {0.5f};
        float zoomMax {4.0f};
        // Shader effects (SDL GPU render API only)
        int colorGrade {0};       // ColorGrade: 0 off, 1 natural, 2 vivid, 3 cinematic, 4 dusk, 5 night
        bool glow {true};         // glow of explosions, shots and projectiles
        bool sharpPixels {false}; // pixel-art anti-aliased upscaling instead of plain nearest / linear

        // Audio
        int musicVolume {96}; // 0..128
        int fxVolume {128};   // 0..128
        bool muted {false};

        // Network game
        std::string playerName {"Player"};
        std::string lastAddress; // "host" or "host:port" of the last direct connection

        // Game
        std::string language {"en"};
        int difficulty {1}; // 0 = easy .. 3 = hardcore

        static constexpr int kArmyColors = 8;
        static constexpr float kZoomLowest = 0.5f;
        static constexpr float kZoomHighest = 4.0f;
        static constexpr int kMinWindowW = 800;
        static constexpr int kMinWindowH = 480;

        // Brings every value into its valid range
        void clamp();

        bool load(const std::string &path);
        bool save(const std::string &path) const;

        // settings.json in the per-user directory
        static std::string defaultPath();
    };

} // namespace OpenArtOfWar
