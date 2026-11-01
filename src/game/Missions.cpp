#include "game/Missions.hpp"

namespace OpenArtOfWar
{

    const std::vector<MissionDef> &Missions::Campaign(Faction faction)
    {
        return faction == Faction::Confederation ? confederation() : resistance();
    }

    const MissionDef *Missions::Find(Faction faction, int missionId)
    {
        const auto &list = Campaign(faction);
        return missionId >= 0 && missionId < static_cast<int>(list.size()) ? &list[missionId] : nullptr;
    }

} // namespace OpenArtOfWar
