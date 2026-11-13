#pragma once

#include "ui/UiKit.hpp"
#include <algorithm>

namespace OpenArtOfWar::menu_detail
{
inline ui::UiRect TouchMainMenuItemRect(int index, int windowW, int windowH)
{
    constexpr int count = 10;
    const int left = 28;
    const int maxRightW = std::max(240, windowW - left - 24);
    const int width = std::min(maxRightW, std::clamp(windowW / 3, 280, 380));
    const int titleTop = std::max(14, windowH / 28);
    const int minTop = titleTop + 102;
    const int playerW = std::min(windowW - 48, 440);
    const int playerX = std::max(24, (windowW - playerW) / 2);
    const int playerTop = windowH - 134; // touch player panel (80) + bottom margin (20) + seek gap (34)
    const int bottomReserve = (left + width > playerX - 12) ? (windowH - (playerTop - 12)) : 16;
    const int availH = std::max(count * 24, windowH - minTop - bottomReserve);
    const int step = std::max(24, std::min(56, availH / count));
    const int top = minTop + std::max(0, (availH - count * step) / 2);
    return {left, top + index * step, width, std::max(1, step - 6)};
}
} // namespace OpenArtOfWar::menu_detail
