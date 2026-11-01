#include "game/MissionDialogs.hpp"

#include "core/Localization.hpp"

namespace OpenArtOfWar
{

namespace
{
// The translation of a key, or empty when the catalog does not have it (gettext then returns the key itself)
std::string Lookup(const std::string &key)
{
    std::string value = Localization::Tr(key);
    return value == key ? std::string() : value;
}

constexpr int kConfederationBase = 100;
constexpr int kMissionsPerCampaign = 7;
constexpr int kMaxLines = 64;
} // namespace

std::string MissionDialogs::KeyPrefix(int dialogId)
{
    if (dialogId >= DialogMission_res_ID1 && dialogId <= DialogMission_res_ID7)
        return "dialog.res." + std::to_string(dialogId);
    if (dialogId >= DialogMission_conf_ID1 && dialogId <= DialogMission_conf_ID7)
        return "dialog.conf." + std::to_string(dialogId - kConfederationBase);
    return std::string();
}

int MissionDialogs::IdForMission(Faction campaign, int missionIndex)
{
    if (missionIndex < 0 || missionIndex >= kMissionsPerCampaign)
        return 0;
    if (campaign == Faction::Confederation)
        return DialogMission_conf_ID1 + missionIndex;
    if (campaign == Faction::Resistance)
        return DialogMission_res_ID1 + missionIndex;
    return 0;
}

MissionDialog MissionDialogs::Get(int dialogId)
{
    MissionDialog dialog;
    const std::string prefix = KeyPrefix(dialogId);
    if (prefix.empty())
        return dialog;
    dialog.id = dialogId;
    dialog.title = Lookup(prefix + ".title");
    for (int i = 1; i <= kMaxLines; ++i)
    {
        const std::string line = prefix + "." + std::to_string(i);
        std::string text = Lookup(line + ".text");
        if (text.empty())
            break;
        dialog.lines.push_back({Lookup(line + ".speaker"), std::move(text)});
    }
    return dialog;
}

} // namespace OpenArtOfWar
