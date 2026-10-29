#pragma once

#if defined(AOW_DEBUG)

#include <SDL3/SDL.h>
#include <string>

namespace OpenArtOfWar
{

    class AIController;

    class TileMap;
    class EntityManager;
    class Economy;
    class MissionController;
    class SoundEngine;
    class Camera;

    class DebugOverlay
    {
    public:
        DebugOverlay(SDL_Window *window, SDL_Renderer *renderer);
        ~DebugOverlay();

        void processEvent(const SDL_Event &event);
        void toggle()
        {
            m_visible = !m_visible;
        }
        bool isVisible() const
        {
            return m_visible;
        }

        void render(TileMap &map, EntityManager &entities, Economy &economy, MissionController &mission, SoundEngine &sound, Camera &camera, AIController &ai, float fps);

        bool isFogDisabled() const
        {
            return m_disableFog;
        }

    private:
        SDL_Window *m_window {nullptr};
        SDL_Renderer *m_renderer {nullptr};
        bool m_visible {false};
        bool m_disableFog {false};

        // AI control panel
        void renderAiPanel(AIController &ai, EntityManager &entities);
        int m_aiCreditsEdit {0};
        bool m_aiCreditsEditing {false};
        int m_thoughtFilter {0}; // 0 = all categories
        bool m_thoughtAutoScroll {true};
        size_t m_thoughtsSeen {0};
    };

} // namespace OpenArtOfWar

#endif // AOW_DEBUG