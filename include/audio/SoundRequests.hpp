#pragma once

#include <string>
#include <vector>

namespace OpenArtOfWar
{

    // Sounds the interface asks for (menus, sidebar, orders), played by the Application once per frame through the
    // sound effects mixer. Main thread only. Event names are those of assets/data/sfx.json; names starting with '@'
    // are unit replies the Application resolves for the selected units: @select, @move, @attack, @hold.
    namespace SoundRequests
    {
        struct Request
        {
            std::string event;
            bool announcer {false}; // queued announcer line
        };

        inline std::vector<Request> &queue()
        {
            static std::vector<Request> q;
            return q;
        }
        inline void play(const char *event)
        {
            if(queue().size() < 64) // nobody plays them (headless): they stop piling up
                queue().push_back({event, false});
        }
        inline void announce(const char *event)
        {
            if(queue().size() < 64)
                queue().push_back({event, true});
        }
        inline std::vector<Request> take()
        {
            std::vector<Request> out;
            out.swap(queue());
            return out;
        }

        // A clickable control was under the mouse when it was drawn this frame: a click there sounds (the
        // widgets mark it, the Application plays ui_click on the next mouse press)
        inline bool &hoverFlag()
        {
            static bool hovered = false;
            return hovered;
        }
        inline void markHover()
        {
            hoverFlag() = true;
        }
        inline bool takeHover()
        {
            const bool h = hoverFlag();
            hoverFlag() = false;
            return h;
        }
    } // namespace SoundRequests

} // namespace OpenArtOfWar
