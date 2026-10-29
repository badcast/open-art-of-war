#include "game/Sides.hpp"
#include "game/Players.hpp"

namespace OpenArtOfWar
{

    // The local player's slot decides (see Players); a single-player mission sets up both slots here
    Faction playerFaction()
    {
        return Players::factionOf(Players::local());
    }
    Faction aiFaction()
    {
        return opposingFaction(playerFaction());
    }
    void setPlayerFaction(Faction faction)
    {
        Players::setupSinglePlayer(faction);
    }

} // namespace OpenArtOfWar
