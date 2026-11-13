#pragma once

#include "ui/UiKit.hpp"
#include <algorithm>

namespace OpenArtOfWar::menu_detail
{
inline ui::UiRect TouchMainMenuItemRect(int index, int windowW, int windowH)
{
    constexpr int count = 10;
    const int left = 38;
    const int width = std::max(1, std::min(360, windowW - left - 24));
    const int step = std::max(1, std::min(56, (windowH - 40) / count));
    const int top = std::max(8, (windowH - count * step) / 2);
    return {left, top + index * step, width, std::max(1, step - 4)};
}
} // namespace OpenArtOfWar::menu_detail
