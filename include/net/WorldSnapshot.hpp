#pragma once

// State of the world as the authority sends it to a replica (TASKNEW.md, steps 2-4): plain data plus a versioned byte
// encoding. No pointers into the simulation, so it can be built, sent, stored and compared on its own. It carries what
// the replica needs to draw the world and run the HUD; behaviour state (path, timers, AI) never leaves the authority.
//
// Still missing (listed for the steps that fill them): the extraction from EntityManager/Economy (step 2b),
// applying it to a replica's EntityManager (step 3), walls / mines / research / visual events (steps 3 and 5).

#include <cstdint>
#include <string>
#include <vector>

namespace OpenArtOfWar
{

class EntityManager;
class TileMap;

struct ProductionSnap
{
    std::int32_t archetype {0};
    std::uint16_t progressPermille {0}; // 0..1000 of the item's build time
    bool operator==(const ProductionSnap &o) const
    {
        return archetype == o.archetype && progressPermille == o.progressPermille;
    }
};

struct EntitySnap
{
    std::uint32_t id {0};
    bool isBuilding {false};
    std::int32_t type {0};      // UnitType
    std::int32_t archetype {0}; // unit archetype (units only)
    std::int32_t rawBuilding {-1};
    std::uint8_t faction {0};
    std::uint8_t owner {0};
    std::int32_t tileX {0};
    std::int32_t tileY {0};
    float subX {0.0f}; // fine offset in pixels, sent in 1/16 pixel steps
    float subY {0.0f};
    std::int32_t hp {0};
    std::int32_t maxHp {0};
    std::uint8_t state {0}; // ActionState
    bool alive {true};
    std::int32_t facing {0};
    std::int32_t turret {0};
    std::uint32_t target {0};
    std::int32_t orderX {-1};
    std::int32_t orderY {-1};
    bool siege {false};
    // Buildings
    std::uint16_t buildPermille {1000};
    bool powered {true};
    bool producing {false};
    bool repairing {false};
    std::int32_t rallyX {-1};
    std::int32_t rallyY {-1};
    std::vector<ProductionSnap> queue;

    bool operator==(const EntitySnap &o) const;
};

// What happened since the last snapshot (visuals and sounds only, nothing the world is built from)
struct CueSnap
{
    std::uint8_t cue {0};
    float x {0.0f};
    float y {0.0f};
    std::uint8_t owner {255};
    std::string sound; // the event name of the character / weapon, empty: the cue's own
    float gain {1.0f};
    bool operator==(const CueSnap &o) const
    {
        return cue == o.cue && x == o.x && y == o.y && owner == o.owner && sound == o.sound && gain == o.gain;
    }
};
struct BlastSnap
{
    float x {0.0f};
    float y {0.0f};
    float size {1.0f};
    bool operator==(const BlastSnap &o) const
    {
        return x == o.x && y == o.y && size == o.size;
    }
};
struct ExplosionSnap
{
    float x {0.0f};
    float y {0.0f};
    std::int32_t anim {0};
    float delay {0.0f};
    bool operator==(const ExplosionSnap &o) const
    {
        return x == o.x && y == o.y && anim == o.anim && delay == o.delay;
    }
};
struct GroundMarkSnap
{
    float x {0}, y {0};
    std::int32_t composite {0};
    float seconds {0};
    bool operator==(const GroundMarkSnap &o) const
    {
        return x == o.x && y == o.y && composite == o.composite && seconds == o.seconds;
    }
};
struct DeathSnap
{
    std::uint32_t id {0};
    std::int32_t tileX {0}, tileY {0};
    float subX {0}, subY {0}, timer {0};
    std::int32_t sequence {-1}, wreckComposite {-1}, wreckStages {0};
    bool flame {false};
    bool operator==(const DeathSnap &o) const
    {
        return id == o.id && tileX == o.tileX && tileY == o.tileY && subX == o.subX && subY == o.subY
               && timer == o.timer && sequence == o.sequence && wreckComposite == o.wreckComposite
               && wreckStages == o.wreckStages && flame == o.flame;
    }
};
// A shot in flight, as it is drawn
struct ProjectileSnap
{
    std::uint8_t type {0};
    float x {0.0f};
    float y {0.0f};
    std::uint8_t direction {0};
    std::uint8_t dir32 {0};
    std::uint8_t faction {0};
    std::uint8_t owner {0};
    bool operator==(const ProjectileSnap &o) const
    {
        return type == o.type && x == o.x && y == o.y && direction == o.direction && dir32 == o.dir32
               && faction == o.faction && owner == o.owner;
    }
};

struct WallSnap
{
    std::int16_t x {0};
    std::int16_t y {0};
    std::uint8_t faction {0};
    std::uint8_t hpPercent {100};
    std::uint8_t owner {0};
    bool operator==(const WallSnap &o) const
    {
        return x == o.x && y == o.y && faction == o.faction && hpPercent == o.hpPercent && owner == o.owner;
    }
};

struct EconomySnap
{
    std::uint8_t player {0};
    std::int32_t credits {0};
    std::int32_t powerOut {0};
    std::int32_t powerUse {0};
    bool operator==(const EconomySnap &o) const
    {
        return player == o.player && credits == o.credits && powerOut == o.powerOut && powerUse == o.powerUse;
    }
};

struct ResearchSnap
{
    std::uint8_t player {0};
    std::uint32_t completed {0};
    std::int32_t active {-1};
    float timer {0.0f};
    float duration {20.0f};
    bool operator==(const ResearchSnap &o) const
    {
        return player == o.player && completed == o.completed && active == o.active && timer == o.timer
               && duration == o.duration;
    }
};

struct MineSnap
{
    std::int32_t id {0}, tileX {0}, tileY {0}, kind {0};
    std::uint8_t owner {0};
    float armLeft {0.0f};
    bool operator==(const MineSnap &o) const
    {
        return id == o.id && tileX == o.tileX && tileY == o.tileY && kind == o.kind && owner == o.owner
               && armLeft == o.armLeft;
    }
};

struct WorldSnapshot
{
    static constexpr std::uint8_t kVersion = 9;

    std::uint32_t tick {0};
    std::uint32_t baseTick {0};
    bool full {true};                       // false: only what changed since the tick the client acknowledged
    std::vector<EntitySnap> entities;       // sorted by id
    std::vector<std::uint32_t> removed;     // sorted ids absent from the delta base
    std::vector<std::uint32_t> hiddenAlive; // alive outside the viewer's sight
    std::vector<EconomySnap> economies;
    std::vector<ResearchSnap> research;
    std::vector<MineSnap> mines; // complete visible list in every snapshot
    std::uint8_t result {0}; // the match for this recipient: 0 goes on, 1 victory, 2 defeat (decided by the authority)
    bool hasWalls {false};   // the wall list below is valid (sent when it changed and once a second)
    std::vector<WallSnap> walls;             // every wall tile, row by row
    std::vector<ProjectileSnap> projectiles; // the whole list every time
    std::vector<CueSnap> cues;
    std::vector<BlastSnap> blasts;
    std::vector<ExplosionSnap> explosions;
    std::vector<DeathSnap> deaths;
    std::vector<GroundMarkSnap> groundMarks;

    // The entities of a world (every one, no fog filter yet: step 6), sorted by id. Economies come from Simulation.
    static WorldSnapshot CaptureEntities(const EntityManager &entities, std::uint32_t tick);
    // Both snapshots must describe complete states for the same viewer.
    WorldSnapshot DeltaFrom(const WorldSnapshot &base) const;
    bool ExpandFrom(const WorldSnapshot &base);
    void FilterForPlayer(const EntityManager &entities, const TileMap &map, std::uint8_t player);
    // Walls of the map into / out of the snapshot (a replica takes the whole list over)
    void CaptureWalls(const TileMap &map);
    void ApplyWalls(TileMap &map) const;
    // Authority: adds the shots in flight and takes (empties) the events collected since the last call
    void CaptureEffects(EntityManager &entities);

    // Bytes for AuthorityState::bytes; Decode refuses another version, a damaged or truncated buffer
    std::vector<std::uint8_t> Encode() const;
    static bool Decode(const std::uint8_t *data, std::size_t size, WorldSnapshot &out);

    bool operator==(const WorldSnapshot &o) const;
};

} // namespace OpenArtOfWar
