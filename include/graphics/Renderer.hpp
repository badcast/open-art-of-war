#pragma once

#include <unordered_map>
#include <utility>

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

    void Clear(SDL_Color clearColor = {0, 0, 0, 255});
    void Present();

    // Direct Primitive Sprites & Composites (World Space)
    void DrawRawSpriteWorld(
        int spriteId, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance);

    void DrawCompositeWorld(
        int compId,
        float worldX,
        float worldY,
        const Camera &camera,
        Faction faction = Faction::Resistance,
        int depth = 0);

    void DrawAnimSequenceWorld(
        int animId, int frame, float worldX, float worldY, const Camera &camera, Faction faction = Faction::Resistance);
    void DrawAnimSequenceWorldRotated(
        int animId,
        int frame,
        float worldX,
        float worldY,
        float angleRad,
        const Camera &camera,
        Faction faction = Faction::Resistance);

    // RTS High-Level Entity Drawing
    void DrawTileWorld(
        int tilesetId, int tileIndex, float worldX, float worldY, const Camera &camera, int globalAnimTick = 0);

    void DrawDecorWorld(
        int tilesetId,
        int decorType,
        float worldX,
        float worldY,
        const Camera &camera,
        int animFrame = 0,
        int layer = -1);

    // Trees, groves and rocks (decoration building ids 31..82) stand up from the ground: their layer 0 (ground,
    // trunks) is drawn flat with the terrain, their layer 1 (crowns) in the depth-sorted pass with units, so a unit
    // behind a tree is covered by its crown. Returns the footprint rows of such a decoration, 0 for flat ones
    // (bridges, cliffs, landmarks).
    int TallDecorRows(int tilesetId, int decorType) const;

    // Animated parts of the next buildings drawn (smoke, lights): hidden while a building does not work
    void SetAnimatedPartsVisible(bool visible)
    {
        m_animatedPartsVisible = visible;
    }
    bool AnimatedPartsVisible() const
    {
        return m_animatedPartsVisible;
    }

    struct LastDrawnBuildingInfo
    {
        int buildingType {-1};
        int animFrame {0};
        bool animatedPartsVisible {true};
    };
    LastDrawnBuildingInfo lastDrawnBuilding;
    void DrawBuildingWorld(
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
    static void DefaultBuildingWreck(int width, int height, int &outComposite, int &outStages);

    // How far the pictured middle of a composite (the box around its sprites) lies from the middle of its 180x120
    // canvas, in pixels: a wreck is drawn shifted back by it, so it sits in the middle of the place of the object
    void CompositeCenterOffset(int compId, float &outDx, float &outDy);
    // Draws a composite so that the middle of what it shows is at (centerX, centerY)
    void DrawCompositeCentered(int compId, float centerX, float centerY, const Camera &camera, Faction faction);

    void DrawUnitWorld(
        int unitTypeId,
        int actionState,
        int facingDir,
        int animStep,
        float worldX,
        float worldY,
        const Camera &camera,
        Faction faction = Faction::Resistance,
        int targetFacing = -1,
        bool isFlaming = true,
        int customWreckComp = -1,
        int customWreckStages = -1,
        int customDeathSeq = -1,
        int flashMode
        = 0, // muzzle flash layer: 0 loops, 1 plays once (then the idle layer shows), 2 plays once and holds
        int flashFrames = 0); // frames of the flash (0 = the whole animation)

    // Projectiles & FX (Authentic 32-direction shells, 16-direction rockets, flame stream & smoke)
    void DrawProjectileWorld(
        int baseCompId,
        int dir,
        float worldX,
        float worldY,
        const Camera &camera,
        Faction faction = Faction::Resistance,
        int numDirs = 16);
    void DrawSmokePuffWorld(int frame, float worldX, float worldY, const Camera &camera);

    // Geometric Primitives (World Space)
    void FillRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color);
    void DrawRectWorld(float wx, float wy, float ww, float wh, const Camera &camera, SDL_Color color);
    // Selection effect: a soft glowing bubble (an ellipse of half sizes `halfW` x `halfH` world pixels around the
    // centre point) in `color`, pulsing; `brighten` > 0 adds a light veil over the object (additive), so a building
    // glows
    void DrawSelectionBubble(
        float centerX, float centerY, float halfW, float halfH, const Camera &camera, SDL_Color color, float brighten);
    void DrawLineWorld(float wx1, float wy1, float wx2, float wy2, const Camera &camera, SDL_Color color);

    // Smooth outlines in screen space, each one blended geometry draw (no doubled alpha where segments meet):
    // a ring / ellipse of `thickness` px around (cx, cy), and a line of `width` px
    void DrawRingScreen(float cx, float cy, float rx, float ry, float thickness, SDL_Color color, int segments);
    void DrawThickLineScreen(float x0, float y0, float x1, float y1, float width, SDL_Color color);

    // Screen Space Drawing (UI & HUD)
    void FillRectScreen(int x, int y, int w, int h, SDL_Color color);
    void DrawRectScreen(int x, int y, int w, int h, SDL_Color color);
    void DrawLineScreen(int x1, int y1, int x2, int y2, SDL_Color color);
    void DrawTextureScreen(
        SDL_Texture *tex, int x, int y, int w = 0, int h = 0, int anchor = Anchor::TOP | Anchor::LEFT);
    void DrawRawSpriteScreen(
        int spriteId, int screenX, int screenY, float scale = 1.0f, Faction faction = Faction::Resistance);
    void DrawCompositeScreen(
        int compId, int screenX, int screenY, float scale = 1.0f, Faction faction = Faction::Resistance, int depth = 0);

    // Clipping
    void SetClipRect(const SDL_Rect &rect);
    void ResetClipRect();

    SDL_Renderer *GetSdlRenderer() const
    {
        return m_renderer;
    }

    // Shader effects of the SDL GPU renderer (nullptr or unavailable: SDL's own drawing)
    void SetEffects(ShaderEffects *effects)
    {
        m_effects = effects;
    }
    // Army colour of a player: its units take this colour instead of the classic red / blue sheets
    // (only with shader effects; the sheets themselves stay as they are)
    void SetArmyColor(PlayerId player, bool custom, SDL_Color color);
    // The next sprites also shine: the sprite is drawn once more, added in this colour (strength 0..1; 0 = off)
    void SetSpriteGlow(SDL_Color color, float strength)
    {
        m_glowColor = color;
        m_glowStrength = strength;
    }
    // Whose thing the next draws are (kNoPlayer: nobody's, classic sheets)
    void SetDrawOwner(PlayerId player)
    {
        m_drawOwner = player;
    }
    // Buildings, walls and their icons keep their team trims on the shared sheets (not only the unit sheets):
    // while set, a player with an army colour gets the trims of this faction recoloured on any sheet
    // (Confederation: blue trims, Resistance: red). Faction::Neutral switches it off.
    void SetStructureFaction(Faction faction)
    {
        m_structureTeam = faction == Faction::Confederation ? 1 : (faction == Faction::Resistance ? 2 : 0);
    }
    // Opacity (0..1) of the sprites drawn next (menu effects); every sheet draw applies it, 1 is the normal state
    void SetDrawAlpha(float alpha)
    {
        m_drawAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    }
    // The world is drawn: back to SDL's own shaders for the UI
    void EndWorldPass();

    // Stretches an 8-bit alpha mask (w x h texels, row-major) in one color over a world rectangle,
    // with linear filtering so the texel edges blend smoothly (`smooth` false: hard texel edges)
    void DrawAlphaMaskWorld(
        const std::vector<uint8_t> &alpha,
        int w,
        int h,
        float worldX,
        float worldY,
        float worldW,
        float worldH,
        const Camera &camera,
        SDL_Color color,
        bool smooth = true);

    ~Renderer();

private:
    bool m_animatedPartsVisible {true};
    void ApplyAnchor(int &x, int &y, int w, int h, int anchor) const;
    // Sprite sheet `page` (after the classic faction mapping) for a world draw: switches to the army
    // colour sheet and shader state when that faction has its own colour; `page` becomes the sheet used
    SDL_Texture *BindSheet(int &page, Faction faction);
    // Untextured primitives, fog, UI: no custom shader
    void PlainState();
    void RenderCompositePartsRotated(
        TextureManager *tm,
        SDL_Renderer *sdlRnd,
        int compId,
        float originX,
        float originY,
        float baseOx,
        float baseOy,
        float angleDeg,
        const Camera &camera,
        Faction faction,
        int depth);

    ShaderEffects *m_effects {nullptr};
    bool m_armyCustom[kMaxPlayers] {};
    SDL_Color m_armyColor[kMaxPlayers] {};
    PlayerId m_drawOwner {kNoPlayer};
    float m_drawAlpha {1.0f};
    SDL_Color m_glowColor {255, 255, 255, 255};
    float m_glowStrength {0.0f};
    int m_structureTeam {0};                                             // see setStructureFaction
    std::unordered_map<int, std::pair<float, float>> m_compCenterOffset; // CompositeCenterOffset cache
    bool m_tintedSheet[64] {}; // sheets whose colour mod carries an army colour right now

    SDL_Renderer *m_renderer {nullptr};
    TextureManager *m_texManager {nullptr};
    SDL_Texture *m_maskTexture {nullptr};
    SDL_Texture *m_glowTexture {nullptr}; // white radial falloff with a soft rim (selection bubble)
    int m_maskW {0};
    int m_maskH {0};
    std::vector<uint32_t> m_maskPixels;
};

} // namespace OpenArtOfWar
