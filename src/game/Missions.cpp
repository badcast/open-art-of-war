#include "game/Missions.hpp"

namespace OpenArtOfWar
{

    const std::vector<MissionDef> &Missions::campaign(Faction faction)
    {
        return faction == Faction::Confederation ? confederation() : resistance();
    }

    const MissionDef *Missions::find(Faction faction, int missionId)
    {
        const auto &list = campaign(faction);
        return missionId >= 0 && missionId < static_cast<int>(list.size()) ? &list[missionId] : nullptr;
    }

} // namespace OpenArtOfWar
