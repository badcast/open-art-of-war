#pragma once

#include <cstdlib>

namespace OpenArtOfWar
{

    // Developer switches from the environment (OAOW_NET_BOT, OAOW_NET_TRACE, OAOW_NET_DROP_AT, OAOW_NET_FACTION,
    // OAOW_NET_FACTION_BY_SLOT, OAOW_TEST_*): read only in a debug build (cmake -DAOW_DEBUG=ON). A release build
    // ignores them, so a player's environment can never change how the game behaves.
    inline const char *debugEnv(const char *name)
    {
#if defined(AOW_DEBUG)
        return std::getenv(name);
#else
        (void) name;
        return nullptr;
#endif
    }

} // namespace OpenArtOfWar
