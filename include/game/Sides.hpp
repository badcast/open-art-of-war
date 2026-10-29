#pragma once

#include "core/Config.hpp"

namespace OpenArtOfWar
{

    /**
     * @brief Which faction the human plays and which one the AI commands.
     *
     * Chosen before a mission starts; every "player" / "enemy" decision in the game goes through
     * these helpers instead of assuming Resistance = player and Confederation = AI.
     */
    Faction playerFaction();
    Faction aiFaction();
    void setPlayerFaction(Faction faction);

    inline Faction opposingFaction(Faction faction)
    {
        return faction == Faction::Resistance ? Faction::Confederation : Faction::Resistance;
    }

} // namespace OpenArtOfWar
