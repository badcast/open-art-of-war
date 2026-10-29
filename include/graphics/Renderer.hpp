#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <vector>
#include "core/Config.hpp"
#include "game/Players.hpp"
#include "graphics/Camera.hpp"
#include "graphics/TextureManager.hpp"

namespace OpenArtOfWar
{

    class ShaderEffects;

    class Renderer
    {
    public:
        Renderer(SDL_Renderer *sdlRenderer, TextureManager *textureManager);

        void clear(SDL_Color clearColor = {0, 0, 0, 255});
        void present();

        // Direct Primitive Sprites & Composites (World Space)
        void drawRawSpriteWorld(int spriteId, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance);

        void drawCompositeWorld(int compId, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance, int depth = 0);

        void drawAnimSequenceWorld(int animId, int frame, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance);
        void drawAnimSequenceWorldRotated(int animId, int frame, float worldX, float worldY, float angleRad, const Camera &camera, Faction faction = Faction::Resistance);

        // RTS High-Level Entity Drawing
        void drawTileWorld(int tilesetId, int tileIndex, float worldX, float worldY, const Camera &camera, int globalAnimTick = 0);

        void drawDecorWorld(int tilesetId, int decorType, float worldX, float worldY, const Camera &camera, int animFrame = 0, int layer = -1);

        void drawBuildingWorld(
            int buildingType,
            float worldX,
            float worldY,
            int layer,
            const Camera &camera,
            Faction faction = Faction::Resistance,
            int animFrame = 0,
            float buildProgress = 1.0f,
            bool isDestroyed = false,
            float hpRatio = 1.0f,
            int turretFacing = 0,
            bool isAttacking = false,
            bool isPowered = true,
            int customWreckComp = 0,
            int customWreckStages = 0);
        // Rubble composite family a destroyed building of that size leaves (first stage, and its stage count)
        static void defaultBuildingWreck(int width, int height, int &outComposite, int &outStages);

        void drawUnitWorld(
            int unitTypeId, int actionState, int facingDir, int animStep, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance, int targetFacing = -1, bool isFlaming = true, int customWreckComp = -1, int customWreckStages = -1, int customDeathSeq = -1);

        // Projectiles & FX (Authentic 32-direction shells, 16-direction rockets, flame stream & smoke)
        void drawProjectileWorld(int baseCompId, int dir, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance, int numDirs = 16);
        void drawSmokePuffWorld(int frame, float worldX, float worldY, const Camera &camera);

        // Geometric Primitives (World Space)
        void fillRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color);
        void drawRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color);
        void drawLineWorld(float wx1, float wy1, float wx2, float wy2, const Camera &camera, SDL_Color color);

        // Smooth outlines in screen space, each one blended geometry draw (no doubled alpha where segments meet):
        // a ring / ellipse of `thickness` px around (cx, cy), and a line of `width` px
        void drawRingScreen(float cx, float cy, float rx, float ry, float thickness, SDL_Color color, int segments);
        void drawThickLineScreen(float x0, float y0, float x1, float y1, float width, SDL_Color color);

        // Screen Space Drawing (UI & HUD)
        void fillRectScreen(int x, int y, int w, int h, SDL_Color color);
        void drawRectScreen(int x, int y, int w, int h, SDL_Color color);
        void drawLineScreen(int x1, int y1, int x2, int y2, SDL_Color color);
        void drawTextureScreen(SDL_Texture *tex, int x, int y, int w = 0, int h = 0, int anchor = Anchor::TOP | Anchor::LEFT);
        void drawRawSpriteScreen(int spriteId, int screenX, int screenY, float scale = 1.0f, Faction faction = Faction::Resistance);
        void drawCompositeScreen(int compId, int screenX, int screenY, float scale = 1.0f, Faction faction = Faction::Resistance, int depth = 0);

        // Clipping
        void setClipRect(const SDL_Rect &rect);
        void resetClipRect();

        SDL_Renderer *getSDLRenderer() const
        {
            return m_renderer;
        }

        // Shader effects of the SDL GPU renderer (nullptr or unavailable: SDL's own drawing)
        void setEffects(ShaderEffects *effects)
        {
            m_effects = effects;
        }
        // Army colour of a player: its units take this colour instead of the classic red / blue sheets
        // (only with shader effects; the sheets themselves stay as they are)
        void setArmyColor(PlayerId player, bool custom, SDL_Color color);
        // Whose thing the next draws are (kNoPlayer: nobody's, classic sheets)
        void setDrawOwner(PlayerId player)
        {
            m_drawOwner = player;
        }
        // Opacity (0..1) of the sprites drawn next (menu effects); every sheet draw applies it, 1 is the normal state
        void setDrawAlpha(float alpha)
        {
            m_drawAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        }
        // The world is drawn: back to SDL's own shaders for the UI
        void endWorldPass();

        // Stretches an 8-bit alpha mask (w x h texels, row-major) in one color over a world rectangle,
        // with linear filtering so the texel edges blend smoothly (`smooth` false: hard texel edges)
        void drawAlphaMaskWorld(const std::vector<uint8_t> &alpha, int w, int h, float worldX, float worldY, float worldW, float worldH, const Camera &camera, SDL_Color color, bool smooth = true);

        ~Renderer();

    private:
        void applyAnchor(int &x, int &y, int w, int h, int anchor) const;
        // Sprite sheet `page` (after the classic faction mapping) for a world draw: switches to the army
        // colour sheet and shader state when that faction has its own colour; `page` becomes the sheet used
        SDL_Texture *bindSheet(int &page, Faction faction);
        // Untextured primitives, fog, UI: no custom shader
        void plainState();
        void renderCompositePartsRotated(TextureManager *tm, SDL_Renderer *sdlRnd, int compId, float originX, float originY, float baseOx, float baseOy, float angleDeg, const Camera &camera, Faction faction, int depth);

        ShaderEffects *m_effects {nullptr};
        bool m_armyCustom[kMaxPlayers] {};
        SDL_Color m_armyColor[kMaxPlayers] {};
        PlayerId m_drawOwner {kNoPlayer};
        float m_drawAlpha {1.0f};

        SDL_Renderer *m_renderer {nullptr};
        TextureManager *m_texManager {nullptr};
        SDL_Texture *m_maskTexture {nullptr};
        int m_maskW {0};
        int m_maskH {0};
        std::vector<uint32_t> m_maskPixels;
    };

} // namespace OpenArtOfWar
