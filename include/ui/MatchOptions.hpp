#pragma once

#include "game/BuildLimits.hpp"
#include "game/Capabilities.hpp"

#include "core/Localization.hpp"
#include "game/Match.hpp"
#include "game/Players.hpp"
#include <string>
#include <string_view>
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
        out.push_back(
            u > 0                        ? std::to_string(u)
            : BuildLimits::MaxArmy() > 0 ? std::string(_("Max")) + " (" + std::to_string(BuildLimits::MaxArmy()) + ")"
                                         : std::string(_("Unlimited")));
    return out;
}
// A tech level (data/capabilities.yaml) as shown: the known ones translated, any other by its name
inline std::string TechLevelName(std::string_view level)
{
    if (level == "basic")
        return _("Basic");
    if (level == "standard")
        return _("Standard");
    if (level == "advanced")
        return _("Advanced");
    if (level == "full")
        return _("Full");
    std::string out(level);
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}
// Choices of a tech level combo: the first stands for -1 (the match's level for a player, the map's / the default
// for the match), then the levels of the file; choice i is level i - 1
inline std::vector<std::string> TechLevelChoices(bool forPlayer)
{
    std::vector<std::string> out {forPlayer ? std::string(_("Match")) : std::string(_("Tech: map"))};
    for (const std::string &l : Capabilities::Levels())
        out.push_back(forPlayer ? TechLevelName(l) : std::string(_("Tech")) + ": " + TechLevelName(l));
    return out;
}
inline std::string TechLevelLabel(int level, bool forPlayer)
{
    const auto choices = TechLevelChoices(forPlayer);
    const int i = level + 1;
    return i >= 0 && i < static_cast<int>(choices.size()) ? choices[static_cast<std::size_t>(i)] : choices.front();
}
inline std::string CreditsLabel(int index)
{
    return "$" + std::to_string(kStartingCredits[index < 0 || index > 4 ? kDefaultCreditsIndex : index]);
}

} // namespace OpenArtOfWar::ui
