#include "game/Players.hpp"

namespace OpenArtOfWar::Players
{

    namespace
    {

        struct Table
        {
            std::array<PlayerSlot, kMaxPlayers> slots {};
            PlayerId local {0};

            Table()
            {
                singlePlayer(Faction::Resistance);
            }

            void singlePlayer(Faction human)
            {
                slots = {};
                slots[0] = {true, human, 0, 0, true};
                slots[1] = {true, human == Faction::Resistance ? Faction::Confederation : Faction::Resistance, 1, 0, false};
                local = 0;
            }
        };

        Table &table()
        {
            static Table t;
            return t;
        }

        const PlayerSlot kNobody {};

    } // namespace

    void setupSinglePlayer(Faction human)
    {
        table().singlePlayer(human);
    }

    void setup(const std::array<PlayerSlot, kMaxPlayers> &slots, PlayerId localId)
    {
        table().slots = slots;
        table().local = localId < kMaxPlayers ? localId : 0;
    }

    const PlayerSlot &slot(PlayerId id)
    {
        return id < kMaxPlayers ? table().slots[id] : kNobody;
    }
    bool isActive(PlayerId id)
    {
        return id < kMaxPlayers && table().slots[id].active;
    }
    PlayerId local()
    {
        return table().local;
    }

    int count()
    {
        int n = 0;
        for(const PlayerSlot &s : table().slots)
            n += s.active ? 1 : 0;
        return n;
    }

    Faction factionOf(PlayerId id)
    {
        return id < kMaxPlayers ? table().slots[id].faction : Faction::Neutral;
    }
    uint8_t teamOf(PlayerId id)
    {
        return id < kMaxPlayers ? table().slots[id].team : 0xFF;
    }

    PlayerId ownerOf(Faction faction)
    {
        if(faction == Faction::Neutral)
            return kNoPlayer;
        for(PlayerId i = 0; i < kMaxPlayers; ++i)
        {
            const PlayerSlot &s = table().slots[i];
            if(s.active && s.faction == faction)
                return i;
        }
        return kNoPlayer;
    }

    bool hostile(PlayerId a, PlayerId b)
    {
        if(a == b)
            return false;
        if(a >= kMaxPlayers || b >= kMaxPlayers)
            return true;
        return table().slots[a].team != table().slots[b].team;
    }

} // namespace OpenArtOfWar::Players
