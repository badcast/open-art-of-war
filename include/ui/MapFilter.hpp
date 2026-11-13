#pragma once

#include "game/MissionController.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace OpenArtOfWar::ui
{

// Case-insensitive text for ASCII and Cyrillic (UTF-8)
inline std::string LowerText(std::string_view t)
{
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        if (c == 0xD0 && i + 1 < t.size())
        {
            const unsigned char d = static_cast<unsigned char>(t[i + 1]);
            if (d >= 0x90 && d <= 0x9F)
            {
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
                ++i;
                continue;
            }
            if (d >= 0xA0 && d <= 0xAF)
            {
                out += static_cast<char>(0xD1);
                out += static_cast<char>(d - 0x20);
                ++i;
                continue;
            }
            if (d == 0x81)
            { // Yo
                out += static_cast<char>(0xD1);
                out += static_cast<char>(0x91);
                ++i;
                continue;
            }
        }
        out += (c < 0x80) ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    }
    return out;
}

// Removes the last UTF-8 character
inline void PopLastUtf8(std::string &s)
{
    while (!s.empty())
    {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}

// Search, filters and sort of the list of maps: the same in the custom game window and in the network room
struct MapFilter
{
    std::string search;
    int players {0}; // 0 any, else maps made for exactly that many players
    int size {0};    // 0 any, 1 small, 2 medium, 3 large (by the area of the map)
    int sort {0};    // 0 name, 1 players, 2 size

    // The maps that pass, in the sort order
    std::vector<SkirmishMapInfo> Apply(const std::vector<SkirmishMapInfo> &all) const
    {
        const std::string needle = LowerText(search);
        std::vector<SkirmishMapInfo> out;
        for (const SkirmishMapInfo &m : all)
        {
            const int area = m.width * m.height;
            const int sizeClass = area <= 2400 ? 1 : (area <= 4800 ? 2 : 3);
            if (players > 0 && m.players != players)
                continue;
            if (size > 0 && sizeClass != size)
                continue;
            if (!needle.empty() && LowerText(m.DisplayName()).find(needle) == std::string::npos
                && LowerText(m.name).find(needle) == std::string::npos)
                continue;
            out.push_back(m);
        }
        std::stable_sort(out.begin(), out.end(), [&](const SkirmishMapInfo &a, const SkirmishMapInfo &b) {
            if (sort == 1 && a.players != b.players)
                return a.players < b.players;
            if (sort == 2 && a.width * a.height != b.width * b.height)
                return a.width * a.height < b.width * b.height;
            return LowerText(a.DisplayName()) < LowerText(b.DisplayName());
        });
        return out;
    }

    // 0 (any) and the player counts the maps have
    static std::vector<int> PlayerCounts(const std::vector<SkirmishMapInfo> &all)
    {
        std::vector<int> counts {0};
        for (const SkirmishMapInfo &m : all)
            if (std::find(counts.begin(), counts.end(), m.players) == counts.end())
                counts.push_back(m.players);
        std::sort(counts.begin(), counts.end());
        return counts;
    }
};

} // namespace OpenArtOfWar::ui
