#include "graphics/ShaderEffects.hpp"

#include "OaowShaders.inc" // generated: kShader_<name>_spv / _msl byte arrays (cmake/EmbedShaders.cmake)

#include <iostream>

namespace OpenArtOfWar
{

    namespace
    {

        // Uniform blocks, std140 like the GLSL side
        struct SpriteParams
        {
            float sharp, team, unused0, unused1;
        };
        struct PostParams
        {
            float grade[4];  // exposure, contrast, saturation, vignette
            float tint[4];   // light colour, w: grading on
            float filter[4]; // x: sharp upscaling
        };
        struct BlurParams
        {
            float step[4];
        };

        struct GradePreset
        {
            float exposure, contrast, saturation, vignette;
            float r, g, b;
            float glow; // strength of the glow on the screen: at night it lights up the surroundings
        };

        const GradePreset kPresets[static_cast<int>(ColorGrade::Count)] = {
            {1.00f, 1.00f, 1.00f, 0.00f, 1.00f, 1.00f, 1.00f, 1.0f}, // off
            {1.03f, 1.06f, 1.08f, 0.18f, 1.00f, 1.00f, 1.00f, 1.0f}, // natural
            {1.05f, 1.12f, 1.30f, 0.22f, 1.00f, 1.00f, 1.00f, 1.0f}, // vivid
            {1.00f, 1.10f, 0.85f, 0.35f, 1.05f, 1.00f, 0.92f, 1.0f}, // cinematic
            {0.95f, 1.05f, 0.95f, 0.35f, 1.10f, 0.85f, 0.70f, 1.2f}, // dusk
            {0.60f, 1.10f, 0.70f, 0.45f, 0.65f, 0.75f, 1.00f, 1.6f}, // night
        };

    } // namespace

    ShaderEffects::~ShaderEffects()
    {
        shutdown();
    }

    SDL_GPUShader *ShaderEffects::createShader(const unsigned char *spv, size_t spvSize, const unsigned char *msl, size_t mslSize, int uniformBuffers)
    {
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(m_device);
        SDL_GPUShaderCreateInfo info;
        SDL_zero(info);
        if((formats & SDL_GPU_SHADERFORMAT_SPIRV) && spv && spvSize)
        {
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.code = spv;
            info.code_size = spvSize;
            info.entrypoint = "main";
        }
        else if((formats & SDL_GPU_SHADERFORMAT_MSL) && msl && mslSize)
        {
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.code = msl;
            info.code_size = mslSize;
            info.entrypoint = "main0"; // spirv-cross renames main
        }
        else
        {
            return nullptr;
        }
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 1;
        info.num_uniform_buffers = static_cast<Uint32>(uniformBuffers);
        SDL_GPUShader *shader = SDL_CreateGPUShader(m_device, &info);
        if(!shader)
            std::cerr << "[Effects] Shader failed: " << SDL_GetError() << std::endl;
        return shader;
    }

    SDL_GPURenderState *ShaderEffects::createState(SDL_GPUShader *shader)
    {
        SDL_GPURenderStateCreateInfo info;
        SDL_zero(info);
        info.fragment_shader = shader;
        SDL_GPURenderState *state = SDL_CreateGPURenderState(m_renderer, &info);
        if(!state)
            std::cerr << "[Effects] Render state failed: " << SDL_GetError() << std::endl;
        return state;
    }

    bool ShaderEffects::init(SDL_Renderer *renderer)
    {
        shutdown();
        m_renderer = renderer;
        m_device = renderer ? SDL_GetGPURendererDevice(renderer) : nullptr;
        if(!m_device)
            return false; // not the SDL GPU renderer: no custom shaders

        m_spriteShader = createShader(kShader_sprite_spv, kShader_sprite_spv_size, kShader_sprite_msl, kShader_sprite_msl_size, 1);
        m_postShader = createShader(kShader_post_spv, kShader_post_spv_size, kShader_post_msl, kShader_post_msl_size, 1);
        m_glowShader = createShader(kShader_glow_spv, kShader_glow_spv_size, kShader_glow_msl, kShader_glow_msl_size, 0);
        m_blurShader = createShader(kShader_blur_spv, kShader_blur_spv_size, kShader_blur_msl, kShader_blur_msl_size, 1);
        if(!m_spriteShader || !m_postShader || !m_glowShader || !m_blurShader)
        {
            std::cerr << "[Effects] The GPU takes none of the built shader formats: effects off" << std::endl;
            shutdown();
            return false;
        }
        m_spriteState = createState(m_spriteShader);
        m_teamState = createState(m_spriteShader);
        m_teamRedState = createState(m_spriteShader);
        m_postState = createState(m_postShader);
        m_glowState = createState(m_glowShader);
        m_blurH = createState(m_blurShader);
        m_blurV = createState(m_blurShader);
        if(!m_spriteState || !m_teamState || !m_teamRedState || !m_postState || !m_glowState || !m_blurH || !m_blurV)
        {
            shutdown();
            return false;
        }
        m_available = true;
        updateSpriteUniforms();
        std::cout << "[Effects] Shader effects ready" << std::endl;
        return true;
    }

    void ShaderEffects::shutdown()
    {
        if(m_renderer && m_current)
            SDL_SetGPURenderState(m_renderer, nullptr);
        m_current = nullptr;
        m_locked = false;
        for(SDL_Texture **t : {&m_scene, &m_glowA, &m_glowB})
        {
            if(*t)
                SDL_DestroyTexture(*t);
            *t = nullptr;
        }
        m_sceneW = m_sceneH = m_glowW = m_glowH = m_glowBW = m_glowBH = 0;
        for(SDL_GPURenderState **s : {&m_spriteState, &m_teamState, &m_teamRedState, &m_postState, &m_glowState, &m_blurH, &m_blurV})
        {
            if(*s)
                SDL_DestroyGPURenderState(*s);
            *s = nullptr;
        }
        for(SDL_GPUShader **s : {&m_spriteShader, &m_postShader, &m_glowShader, &m_blurShader})
        {
            if(*s && m_device)
                SDL_ReleaseGPUShader(m_device, *s);
            *s = nullptr;
        }
        m_available = false;
        m_device = nullptr;
    }

    void ShaderEffects::configure(ColorGrade grade, bool glow, bool sharp)
    {
        m_grade = grade;
        m_glow = glow;
        if(m_sharp != sharp)
        {
            m_sharp = sharp;
            updateSpriteUniforms();
        }
    }

    void ShaderEffects::updateSpriteUniforms()
    {
        if(!m_available)
            return;
        SpriteParams p {m_sharp ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        SDL_SetGPURenderStateFragmentUniforms(m_spriteState, 0, &p, sizeof p);
        p.team = 1.0f;
        SDL_SetGPURenderStateFragmentUniforms(m_teamState, 0, &p, sizeof p);
        p.team = 2.0f;
        SDL_SetGPURenderStateFragmentUniforms(m_teamRedState, 0, &p, sizeof p);
    }

    void ShaderEffects::setState(SDL_GPURenderState *state)
    {
        if(state == m_current)
            return;
        SDL_SetGPURenderState(m_renderer, state);
        m_current = state;
    }

    void ShaderEffects::useSpriteState(int team)
    {
        if(!m_available || m_locked)
            return;
        if(team == 1)
            setState(m_teamState);
        else if(team == 2)
            setState(m_teamRedState);
        else
            setState(m_sharp ? m_spriteState : nullptr);
    }

    void ShaderEffects::useDefaultState()
    {
        if(!m_available || m_locked)
            return;
        setState(nullptr);
    }

    SDL_Texture *ShaderEffects::ensureTarget(SDL_Texture *&tex, int &curW, int &curH, int w, int h, SDL_ScaleMode mode)
    {
        w = w > 1 ? w : 1;
        h = h > 1 ? h : 1;
        if(!tex || curW != w || curH != h)
        {
            if(tex)
                SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
            curW = w;
            curH = h;
        }
        if(tex)
            SDL_SetTextureScaleMode(tex, mode);
        return tex;
    }

    SDL_Texture *ShaderEffects::sceneTarget(int w, int h)
    {
        if(!m_available)
            return nullptr;
        return ensureTarget(m_scene, m_sceneW, m_sceneH, w, h, SDL_SCALEMODE_LINEAR);
    }

    void ShaderEffects::beginGlow(int w, int h)
    {
        if(!glow())
            return;
        const bool resized = w != m_glowW || h != m_glowH;
        if(!ensureTarget(m_glowA, m_glowW, m_glowH, w, h, SDL_SCALEMODE_LINEAR) || !ensureTarget(m_glowB, m_glowBW, m_glowBH, w, h, SDL_SCALEMODE_LINEAR))
        {
            return;
        }
        if(resized)
        {
            BlurParams p {{1.0f / m_glowW, 0.0f, 0.0f, 0.0f}};
            SDL_SetGPURenderStateFragmentUniforms(m_blurH, 0, &p, sizeof p);
            p = BlurParams {{0.0f, 1.0f / m_glowH, 0.0f, 0.0f}};
            SDL_SetGPURenderStateFragmentUniforms(m_blurV, 0, &p, sizeof p);
        }
        SDL_SetRenderTarget(m_renderer, m_glowA);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
        SDL_RenderClear(m_renderer);
        setState(m_glowState);
        m_locked = true;
    }

    void ShaderEffects::endGlow()
    {
        if(!m_locked)
            return;
        m_locked = false;
        // Two passes of the separable blur: A -> B horizontally, B -> A vertically
        SDL_SetTextureBlendMode(m_glowA, SDL_BLENDMODE_NONE);
        SDL_SetTextureBlendMode(m_glowB, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(m_renderer, m_glowB);
        setState(m_blurH);
        SDL_RenderTexture(m_renderer, m_glowA, nullptr, nullptr);
        SDL_SetRenderTarget(m_renderer, m_glowA);
        setState(m_blurV);
        SDL_RenderTexture(m_renderer, m_glowB, nullptr, nullptr);
        setState(nullptr);
    }

    void ShaderEffects::composite(const SDL_FRect &dst, const SDL_FRect &glowDst, bool smoothing)
    {
        if(!m_available || !m_scene)
            return;
        const GradePreset &g = kPresets[static_cast<int>(m_grade)];
        PostParams p {{g.exposure, g.contrast, g.saturation, g.vignette}, {g.r, g.g, g.b, m_grade != ColorGrade::Off ? 1.0f : 0.0f}, {m_sharp ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
        SDL_SetGPURenderStateFragmentUniforms(m_postState, 0, &p, sizeof p);
        // Sharp upscaling needs linear samples; without it the reduced picture keeps the chosen smoothing
        SDL_SetTextureScaleMode(m_scene, (m_sharp || smoothing) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(m_scene, SDL_BLENDMODE_NONE);
        setState(m_postState);
        SDL_RenderTexture(m_renderer, m_scene, nullptr, &dst);
        setState(nullptr);

        if(glow() && m_glowA)
        {
            const Uint8 strength = static_cast<Uint8>(g.glow >= 1.0f ? 255 : g.glow * 255.0f);
            SDL_SetTextureBlendMode(m_glowA, SDL_BLENDMODE_ADD);
            SDL_SetTextureColorMod(m_glowA, strength, strength, strength);
            // Twice at night / dusk: the glow lights up the surroundings
            const int passes = g.glow > 1.0f ? 2 : 1;
            for(int i = 0; i < passes; ++i)
                SDL_RenderTexture(m_renderer, m_glowA, nullptr, &glowDst);
        }
    }

} // namespace OpenArtOfWar
