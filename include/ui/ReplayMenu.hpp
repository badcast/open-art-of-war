#pragma once

#include "replay/ReplayPlayer.hpp"
#include "ui/UiKit.hpp"

#include <string>
#include <vector>

namespace OpenArtOfWar
{

class InputManager;
class Renderer;
class BitmapFontRenderer;

// "Open replay": the replays in the user's folder, newest first, with the match of the selected one. Enter or a
// double click opens it, Esc goes back; a file can also be dropped on the window.
class ReplayMenu
{
public:
    struct Result
    {
        bool back {false};
        std::string open; // a file to play
    };

    // Reads the folder again (call when the screen opens)
    void Refresh();
    void SetNotice(const std::string &text)
    {
        m_notice = text;
    }
    Result Update(InputManager &input, int windowW, int windowH);
    void Render(Renderer &renderer, BitmapFontRenderer &font, int windowW, int windowH);

private:
    struct Entry
    {
        ReplayPlayer::Info info;
        std::string name; // file name
        bool ok {false};
        std::string error;
        bool mapMissing {false};
    };
    struct Layout
    {
        ui::UiRect dialog, list, details, openBtn, backBtn;
        int rowH {34};
        int rows {0};
    };
    static Layout MakeLayout(int windowW, int windowH);
    void ClampScroll(int rows);

    std::vector<Entry> m_entries;
    int m_selected {0};
    int m_top {0};
    int m_mouseX {0}, m_mouseY {0};
    std::uint32_t m_lastClickMs {0};
    int m_lastClickRow {-1};
    std::string m_notice;
};

} // namespace OpenArtOfWar
