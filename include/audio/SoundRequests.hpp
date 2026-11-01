#pragma once

#include <string>
#include <vector>

namespace OpenArtOfWar
{

// Sounds the interface asks for (menus, sidebar, orders), played by the Application once per frame through the
// sound effects mixer. Main thread only. Event names are those of assets/data/audio_sfx.json; names starting with '@'
// are unit replies the Application resolves for the selected units: @select, @move, @attack, @hold.
namespace SoundRequests
{
struct Request
{
    std::string event;
    bool announcer {false}; // queued announcer line
};

inline std::vector<Request> &Queue()
{
    static std::vector<Request> q;
    return q;
}
inline void Play(const char *event)
{
    if (Queue().size() < 64) // nobody plays them (headless): they stop piling up
        Queue().push_back({event, false});
}
inline void Announce(const char *event)
{
    if (Queue().size() < 64)
        Queue().push_back({event, true});
}
inline std::vector<Request> Take()
{
    std::vector<Request> out;
    out.swap(Queue());
    return out;
}

// A clickable control was under the mouse when it was drawn this frame: a click there sounds (the
// widgets mark it, the Application plays ui_click on the next mouse press)
inline bool &HoverFlag()
{
    static bool hovered = false;
    return hovered;
}
inline void MarkHover()
{
    HoverFlag() = true;
}
inline bool TakeHover()
{
    const bool h = HoverFlag();
    HoverFlag() = false;
    return h;
}
} // namespace SoundRequests

} // namespace OpenArtOfWar
