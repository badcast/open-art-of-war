#pragma once

#include "game/Players.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace OpenArtOfWar
{

class SoundEngine;
class EntityManager;
class Economy;
class TileMap;
class Camera;
class RadarAlert;

// What the world sounds like: shots, hits, explosions and deaths heard where they happen (quieter and panned
// with the distance from the view, never from under the fog of war), and the announcer telling the local
// player about its base (construction complete, unit ready, unit lost, base under attack, low power, enemy on
// the radar). Only reads the world; nothing here feeds back into the simulation.
class GameSounds
{
public:
    // A new world (entity ids start again): forget what was seen
    void Reset();
    // Once per frame while a match is shown
    void Update(
        SoundEngine &sound,
        EntityManager &entities,
        const Economy &economy,
        const TileMap &map,
        const Camera &camera,
        bool fogEnabled,
        const RadarAlert &radar,
        uint64_t nowMs);
    // Unit replies of the '@' requests (see SoundRequests): by the first selected unit
    void PlayReply(
        SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected, const char *what);
    // The player picked units: a reply when the selection got someone new of the player's own
    void SelectionChanged(SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected);

private:
    struct Seen
    {
        bool dead {false};
        bool complete {false};
        int hp {0};
        PlayerId owner {kNoPlayer};
    };
    std::unordered_map<int, Seen> m_seen;
    bool m_primed {false}; // the first look at a world only records it
    uint32_t m_generation {0xFFFFFFFFu};
    bool m_lowPower {false};
    uint64_t m_radarStart {0};
    std::vector<int> m_selection;
};

} // namespace OpenArtOfWar
