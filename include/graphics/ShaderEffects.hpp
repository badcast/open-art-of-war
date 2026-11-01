#pragma once

#include <SDL3/SDL.h>

namespace OpenArtOfWar
{

    // Colour grading presets of the world picture (graphics settings)
    enum class ColorGrade : int
    {
        Off = 0,
        Natural,
        Vivid,
        Cinematic,
        Dusk,
        Night,
        Count
    };

    // Own colours a player can give the army (index 1..8 of Settings::armyColor; 0 = classic sheets)
    inline SDL_Color ArmyColor(int index)
    {
        static const SDL_Color kColors[8] = {
            {40, 110, 230, 255},
            {210, 40, 30, 255},
            {40, 170, 60, 255},
            {225, 200, 40, 255},
            {150, 60, 200, 255},
            {235, 120, 30, 255},
            {40, 200, 210, 255},
            {225, 225, 230, 255},
        };
        return (index >= 1 && index <= 8) ? kColors[index - 1] : SDL_Color {255, 255, 255, 255};
    }
    inline const char *ArmyColorName(int index)
    {
        static const char *const kNames[9] = {"Classic", "Blue", "Red", "Green", "Yellow", "Purple", "Orange", "Cyan", "White"};
        return (index >= 0 && index <= 8) ? kNames[index] : kNames[0];
    }

    // Shader effects of the SDL GPU renderer (shaders/*.frag). On any other render API, or when the GPU
    // device takes none of the built shader formats, available() is false and the game draws as before.
    //   - world sprites: pixel-art anti-aliased upscaling, army colour (blue team armour in another hue)
    //   - world picture: colour grading / lighting, sharp upscaling of the reduced-resolution picture
    //   - glow: explosions, muzzle flashes and projectiles drawn again into a quarter-size target,
    //     bright pass, separable Gaussian blur, added onto the screen
    // The sprite sheets are never modified: everything happens while sampling them.
    class ShaderEffects
    {
    public:
        ShaderEffects() = default;
        ~ShaderEffects();
        ShaderEffects(const ShaderEffects &) = delete;
        ShaderEffects &operator=(const ShaderEffects &) = delete;

        bool Init(SDL_Renderer *renderer);
        void Shutdown();
        bool Available() const
        {
            return m_available;
        }

        void Configure(ColorGrade grade, bool glow, bool sharp);
        bool Sharp() const
        {
            return m_available && m_sharp;
        }
        bool Glow() const
        {
            return m_available && m_glow;
        }
        // The world goes through an offscreen picture this frame (grading, glow or sharp upscaling)
        bool PostActive() const
        {
            return m_available && (m_grade != ColorGrade::Off || m_glow || m_sharp);
        }

        // Sprite draws: `team` recolours the blue armour into the texture's colour mod
        // team: 0 plain, 1 blue (Confederation) armour recoloured, 2 red (Resistance) armour recoloured
        void UseSpriteState(int team);
        // Untextured primitives, fog, UI: SDL's own shaders
        void UseDefaultState();

        // Offscreen picture of the world, (re)created at w x h
        SDL_Texture *SceneTarget(int w, int h);
        // Glow pass: target of w x h (a fraction of the view), everything drawn until endGlow is bright-passed
        void BeginGlow(int w, int h);
        void EndGlow();
        // The world picture onto `dst` of the screen (current render target must be the screen), then the glow
        // onto `glowDst` (the glow target scaled back to the view)
        void Composite(const SDL_FRect &dst, const SDL_FRect &glowDst, bool smoothing);

    private:
        SDL_GPUShader *CreateShader(const unsigned char *spv, size_t spvSize, const unsigned char *msl, size_t mslSize, int uniformBuffers);
        SDL_GPURenderState *CreateState(SDL_GPUShader *shader);
        void SetState(SDL_GPURenderState *state);
        void UpdateSpriteUniforms();
        SDL_Texture *EnsureTarget(SDL_Texture *&tex, int &curW, int &curH, int w, int h, SDL_ScaleMode mode);

        SDL_Renderer *m_renderer {nullptr};
        SDL_GPUDevice *m_device {nullptr};
        bool m_available {false};

        SDL_GPUShader *m_spriteShader {nullptr};
        SDL_GPUShader *m_postShader {nullptr};
        SDL_GPUShader *m_glowShader {nullptr};
        SDL_GPUShader *m_blurShader {nullptr};
        SDL_GPURenderState *m_spriteState {nullptr};  // AA only
        SDL_GPURenderState *m_teamState {nullptr};    // AA + army colour on blue armour
        SDL_GPURenderState *m_teamRedState {nullptr}; // AA + army colour on red armour
        SDL_GPURenderState *m_postState {nullptr};
        SDL_GPURenderState *m_glowState {nullptr};
        SDL_GPURenderState *m_blurH {nullptr};
        SDL_GPURenderState *m_blurV {nullptr};
        SDL_GPURenderState *m_current {nullptr};
        bool m_locked {false}; // glow pass: sprite draws keep the glow state

        SDL_Texture *m_scene {nullptr};
        int m_sceneW {0}, m_sceneH {0};
        SDL_Texture *m_glowA {nullptr};
        SDL_Texture *m_glowB {nullptr};
        int m_glowW {0}, m_glowH {0};
        int m_glowBW {0}, m_glowBH {0};

        ColorGrade m_grade {ColorGrade::Off};
        bool m_glow {false};
        bool m_sharp {false};
    };

} // namespace OpenArtOfWar
