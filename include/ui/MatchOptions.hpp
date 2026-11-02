#pragma once

#include "core/Localization.hpp"
#include "game/Match.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar::ui
{

// Match options both windows (custom game and network room) offer: bases, starting credits, fog of war
inline std::vector<std::string> LayoutChoices()
{
    return {_("PREBUILT BASES"), _("FROM SCRATCH")};
}
inline std::vector<std::string> CreditsChoices()
{
    std::vector<std::string> out;
    for (int c : kStartingCredits)
        out.push_back("$" + std::to_string(c));
    return out;
}
inline std::string CreditsLabel(int index)
{
    return "$" + std::to_string(kStartingCredits[index < 0 || index > 4 ? kDefaultCreditsIndex : index]);
}

} // namespace OpenArtOfWar::ui
