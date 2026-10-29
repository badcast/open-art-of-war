#pragma once

#include "game/AISquadController.hpp"
#include "game/Economy.hpp"
#include "game/Match.hpp"
#include "game/SimCommand.hpp"
#include <array>
#include <functional>
#include <cstdint>
#include <vector>

namespace OpenArtOfWar
{

    class EntityManager;
    class TileMap;
    class Pathfinder;
    class Economy;
    class AIController;

    // The parts of the world one tick works on
    struct SimContext
    {
        EntityManager &entities;
        TileMap &map;
        Pathfinder &pathfinder;
        Economy &economy;
        AIController &ai;
    };

    // A technology being researched by one side
    struct ResearchProgress
    {
        int active {-1}; // technology index, -1: none
        float timer {0.0f};
        float duration {20.0f};
    };

    // Something the interface wants to hear about (it is not part of the world)
    struct SimEvent
    {
        enum class Type : uint8_t
        {
            ResearchCompleted
        };
        Type type {Type::ResearchCompleted};
        PlayerId player {0};
        int value {0};
    };

    // State of the match that belongs to the players rather than to the entities: queued commands, the
    // marching control groups, the running research. It is part of the simulation, so it lives here and
    // not in the interface; the interface only issues commands and reads it.
    class Simulation
    {
    public:
        Simulation() = default;

        static constexpr uint32_t kDefaultSeed = 0x9e3779b9u;

        // A new match / mission. `seed` starts the gameplay random numbers (shot spread, damage rolls, ...):
        // every peer of a network game uses the host's seed, so they all roll the same numbers.
        void reset(uint32_t seed = kDefaultSeed);

        // A match of several players: seeds the random numbers and gives every other player an economy
        // (the local player's is SimContext::economy)
        void startMatch(const MatchSetup &setup);
        // The economy of a player in this tick
        Economy &economyOf(PlayerId player, SimContext &ctx);
        // Income, power and production of the other players' economies
        void updateEconomies(float dt, SimContext &ctx);

        // From the interface: applied on the next tick that runs (the current one when issued before it).
        // In a network game the sink takes it instead (it goes to the host and comes back in a turn).
        void issue(SimCommand command);
        void setCommandSink(std::function<void(SimCommand)> sink)
        {
            m_sink = std::move(sink);
        }
        // Turn bundle of a network game: applied on `tick`
        void schedule(uint32_t tick, SimCommand command);

        // One tick: due commands in a fixed order, then the research timers
        void applyCommands(SimContext &ctx);
        void updateResearch(float dt, SimContext &ctx);
        // The marching control groups of every player
        void updateSquads(float dt, SimContext &ctx, const ChokepointMap &chokepoints);
        void endTick()
        {
            ++m_tick;
        }

        uint32_t tick() const
        {
            return m_tick;
        }

        // Per player (out-of-range ids get player 0's, never used by the game)
        AISquadController &squads(PlayerId player)
        {
            return m_squads[player % kMaxPlayers];
        }
        const AISquadController &squads(PlayerId player) const
        {
            return m_squads[player % kMaxPlayers];
        }
        const ResearchProgress &research(PlayerId player) const
        {
            return m_research[player % kMaxPlayers];
        }

        std::vector<SimEvent> takeEvents();

        // Every applied command with its tick (a replay of the match)
        void setRecording(bool on)
        {
            m_recording = on;
        }
        const std::vector<TimedCommand> &log() const
        {
            return m_log;
        }

        // 64-bit checksum of the world: equal on every peer of a network game as long as they are in sync
        uint64_t checksum(const SimContext &ctx) const;

        // Settings of the player-commanded marches (shared by every side)
        static const AITacticsConfig &groupTactics();

    private:
        void apply(const SimCommand &cmd, SimContext &ctx);
        // Units of `cmd` that its issuer may command and that can take an order
        std::vector<int> ownUnits(const SimCommand &cmd, const SimContext &ctx) const;
        bool marchAsGroup(PlayerId player, const std::vector<int> &unitIds, int tileX, int tileY, bool attackMove, SimContext &ctx);

        uint32_t m_tick {0};
        std::vector<TimedCommand> m_pending;
        std::function<void(SimCommand)> m_sink;
        std::vector<TimedCommand> m_log;
        bool m_recording {true};
        std::array<AISquadController, kMaxPlayers> m_squads;
        std::array<ResearchProgress, kMaxPlayers> m_research;
        std::vector<SimEvent> m_events;
        std::array<Economy, kMaxPlayers> m_economies;
        uint32_t m_economyMask {0}; // players whose economy lives here
    };

} // namespace OpenArtOfWar
