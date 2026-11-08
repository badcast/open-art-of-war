#pragma once

#include "game/BuildLimits.hpp"

#include "core/Localization.hpp"
#include "game/Match.hpp"
#include "game/Players.hpp"
#include <string>
#include <vector>

namespace OpenArtOfWar::ui
{

// Match options both windows (custom game and network room) offer: bases, starting credits, fog of war
inline std::vector<std::string> LayoutChoices()
{
    return {_("PREBUILT BASES"), _("FROM SCRATCH")};
}
// The time of day of a match: 0 day, 1 night (only drawn and heard)
inline std::vector<std::string> TimeOfDayChoices()
{
    return {_("DAY"), _("NIGHT")};
}
inline std::vector<std::string> CreditsChoices()
{
    std::vector<std::string> out;
    for (int c : kStartingCredits)
        out.push_back("$" + std::to_string(c));
    return out;
}
// The army size limit of a player (kArmyCapUnits): 10 ... 1000 units or unlimited
inline std::vector<std::string> ArmyCapChoices()
{
    std::vector<std::string> out;
    for (int u : kArmyCapUnits)
        out.push_back(u > 0 ? std::to_string(u)
                      : BuildLimits::MaxArmy() > 0
                          ? std::string(_("Max")) + " (" + std::to_string(BuildLimits::MaxArmy()) + ")"
                          : std::string(_("Unlimited")));
    return out;
}
inline std::string CreditsLabel(int index)
{
    return "$" + std::to_string(kStartingCredits[index < 0 || index > 4 ? kDefaultCreditsIndex : index]);
}

} // namespace OpenArtOfWar::ui
