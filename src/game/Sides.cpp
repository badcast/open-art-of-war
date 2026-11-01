#include "game/Sides.hpp"
#include "game/Players.hpp"

namespace OpenArtOfWar
{

// The local player's slot decides (see Players); a single-player mission sets up both slots here
Faction PlayerFaction()
{
    return Players::FactionOf(Players::Local());
}
Faction AiFaction()
{
    return OpposingFaction(PlayerFaction());
}
void SetPlayerFaction(Faction faction)
{
    Players::SetupSinglePlayer(faction);
}

} // namespace OpenArtOfWar
