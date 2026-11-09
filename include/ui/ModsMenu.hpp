#pragma once

#include "core/Mods.hpp"
#include "ui/UiKit.hpp"

#include <string>
#include <vector>

namespace OpenArtOfWar
{

class InputManager;
class Renderer;
class BitmapFontRenderer;

// "Mods": every mod found (core/Mods.hpp), the enabled ones first in their load order. A click on the box (or Space)
// turns one on / off, Up / Down move an enabled one in the order (the lower wins over the upper), Apply puts the
// choice in place and saves it. Esc goes back without applying.
class ModsMenu
{
public:
    struct Result
    {
        bool back {false};
        bool apply {false};
        std::vector<std::string> enabled; // with apply: the ids in load order
    };

    // Looks for the mods again; `enabled`: the ids in use now, in order
    void Refresh(const std::vector<std::string> &enabled);
    // After applying: what came of it (problems, files several mods bring)
    void SetReport(const Mods::ActivateReport &report, const std::string &notice);
    Result Update(InputManager &input, int windowW, int windowH);
    void Render(Renderer &renderer, BitmapFontRenderer &font, int windowW, int windowH);

private:
    struct Entry
    {
        Mods::ModInfo info;
        bool enabled {false};
    };
    struct Layout
    {
        ui::UiRect dialog, list, details, backBtn, applyBtn, upBtn, downBtn;
        int rowH {34};
        int rows {0};
    };
    static Layout MakeLayout(int windowW, int windowH);
    void Toggle(int index);
    void Move(int index, int by);
    std::vector<std::string> EnabledIds() const;

    std::vector<Entry> m_entries;       // enabled ones first, in load order
    std::vector<std::string> m_applied; // what is in place
    Mods::ActivateReport m_report;
    std::string m_notice;
    std::vector<std::string> m_dirs;
    int m_selected {0};
    int m_top {0};
    int m_mouseX {0}, m_mouseY {0};
};

} // namespace OpenArtOfWar
