#pragma once

#include "core/LoaderResources.hpp"

#include <unordered_set>

#include "core/Config.hpp"
#include "graphics/Renderer.hpp"
#include "game/Pathfinder.hpp"
#include "game/Entity.hpp"
#include "game/Unit.hpp"
#include "game/Building.hpp"
#include <vector>
#include <memory>
#include <array>
#include <unordered_map>
#include <string>
#include <string_view>

namespace YAML
{
class Node;
}

namespace OpenArtOfWar
{

class TileMap;
class TextureManager;
struct WorldSnapshot;

/**
 * @brief Container adapter for polymorphic game entities, providing
 * transparent iteration (yielding Entity&) and index access.
 */
class EntityContainer
{
public:
    using Storage = std::vector<std::unique_ptr<Entity>>;

    std::size_t size() const
    {
        return m_items.size();
    }
    bool empty() const
    {
        return m_items.empty();
    }
    void Reserve(std::size_t n)
    {
        m_items.reserve(n);
    }
    void Clear()
    {
        m_items.clear();
    }

    Entity &operator[](std::size_t i)
    {
        return *m_items[i];
    }
    const Entity &operator[](std::size_t i) const
    {
        return *m_items[i];
    }

    void push_back(std::unique_ptr<Entity> e)
    {
        m_items.push_back(std::move(e));
    }

    class iterator
    {
        Storage::iterator m_it;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Entity;
        using difference_type = std::ptrdiff_t;
        using pointer = Entity *;
        using reference = Entity &;

        iterator() = default;
        explicit iterator(Storage::iterator it)
            : m_it(it)
        {}

        Entity &operator*() const
        {
            return **m_it;
        }
        Entity *operator->() const
        {
            return m_it->get();
        }
        iterator &operator++()
        {
            ++m_it;
            return *this;
        }
        iterator operator++(int)
        {
            iterator tmp = *this;
            ++m_it;
            return tmp;
        }
        iterator &operator--()
        {
            --m_it;
            return *this;
        }
        iterator operator--(int)
        {
            iterator tmp = *this;
            --m_it;
            return tmp;
        }
        iterator &operator+=(difference_type n)
        {
            m_it += n;
            return *this;
        }
        iterator &operator-=(difference_type n)
        {
            m_it -= n;
            return *this;
        }
        iterator operator+(difference_type n) const
        {
            return iterator(m_it + n);
        }
        iterator operator-(difference_type n) const
        {
            return iterator(m_it - n);
        }
        difference_type operator-(const iterator &o) const
        {
            return m_it - o.m_it;
        }
        bool operator==(const iterator &o) const
        {
            return m_it == o.m_it;
        }
        bool operator!=(const iterator &o) const
        {
            return m_it != o.m_it;
        }
        bool operator<(const iterator &o) const
        {
            return m_it < o.m_it;
        }
        bool operator>(const iterator &o) const
        {
            return m_it > o.m_it;
        }
        bool operator<=(const iterator &o) const
        {
            return m_it <= o.m_it;
        }
        bool operator>=(const iterator &o) const
        {
            return m_it >= o.m_it;
        }
        Entity &operator[](difference_type n) const
        {
            return **(m_it + n);
        }
    };

    class const_iterator
    {
        Storage::const_iterator m_it;

    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = const Entity;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entity *;
        using reference = const Entity &;

        const_iterator() = default;
        explicit const_iterator(Storage::const_iterator it)
            : m_it(it)
        {}

        const Entity &operator*() const
        {
            return **m_it;
        }
        const Entity *operator->() const
        {
            return m_it->get();
        }
        const_iterator &operator++()
        {
            ++m_it;
            return *this;
        }
        const_iterator operator++(int)
        {
            const_iterator tmp = *this;
            ++m_it;
            return tmp;
        }
        const_iterator &operator--()
        {
            --m_it;
            return *this;
        }
        const_iterator operator--(int)
        {
            const_iterator tmp = *this;
            --m_it;
            return tmp;
        }
        const_iterator &operator+=(difference_type n)
        {
            m_it += n;
            return *this;
        }
        const_iterator &operator-=(difference_type n)
        {
            m_it -= n;
            return *this;
        }
        const_iterator operator+(difference_type n) const
        {
            return const_iterator(m_it + n);
        }
        const_iterator operator-(difference_type n) const
        {
            return const_iterator(m_it - n);
        }
        difference_type operator-(const const_iterator &o) const
        {
            return m_it - o.m_it;
        }
        bool operator==(const const_iterator &o) const
        {
            return m_it == o.m_it;
        }
        bool operator!=(const const_iterator &o) const
        {
            return m_it != o.m_it;
        }
        bool operator<(const const_iterator &o) const
        {
            return m_it < o.m_it;
        }
        bool operator>(const const_iterator &o) const
        {
            return m_it > o.m_it;
        }
        bool operator<=(const const_iterator &o) const
        {
            return m_it <= o.m_it;
        }
        bool operator>=(const const_iterator &o) const
        {
            return m_it >= o.m_it;
        }
        const Entity &operator[](difference_type n) const
        {
            return **(m_it + n);
        }
    };

    iterator begin()
    {
        return iterator(m_items.begin());
    }
    iterator end()
    {
        return iterator(m_items.end());
    }
    const_iterator begin() const
    {
        return const_iterator(m_items.begin());
    }
    const_iterator end() const
    {
        return const_iterator(m_items.end());
    }
    const_iterator cbegin() const
    {
        return const_iterator(m_items.cbegin());
    }
    const_iterator cend() const
    {
        return const_iterator(m_items.cend());
    }

    Storage &Raw()
    {
        return m_items;
    }
    const Storage &Raw() const
    {
        return m_items;
    }

private:
    Storage m_items;
};

struct BuildingBlueprint
{
    int blueprintId {0};
    int buildingIndex {0}; // 0..7 Confederation / 8..14 Resistance buildings, 15 / 25 walls
    int iconCompId {-1};   // cameo composite (sprite handle; production.icon_composite names it)
    UnitType unitType {UnitType::Headquarters};
    std::string name;
    int width {3};
    int height {3};
    int creditCost {400};
    int powerCost {0}; // >0 produces, <0 consumes
    std::string desc;
    bool isWall {false}; // placed as a 1x1 wall segment instead of a building
    int mineKind {-1};   // >= 0: a mine (index into Mines::Specs()), laid on one tile like a wall segment
};

struct WeaponConfig
{
    std::string name {"Primary"};
    ProjectileType projectile {ProjectileType::Bullet};
    int damage {10};
    float range {4.0f};
    float minRange {0.0f};
    float cooldown {0.7f};    // fixed delay between attack cycles (the minimum in random mode)
    float cooldownMax {0.0f}; // > cooldown: random delay in [cooldown, cooldownMax] (cooldown_mode: random)
    int burstCount {1};
    float burstDelay {0.1f};
    float spreadAngle {-1.0f};
    float muzzleOffsetX {0.0f};
    float muzzleOffsetY {0.0f};
    int splashPercent {0};
    bool enabled {true};
    bool requiresResearch {false};
    float maxDistance {0.0f};
    // Sounds of this gun (events of data/audio_sfx.json; empty: the sound of its projectile kind). Only heard:
    // nothing of it is part of the world
    std::string sound;         // the shot (fire_sfx)
    std::string impactSound;   // where it hits (impact_sfx)
    float soundVolume {1.0f};  // times the event's own volume
    std::uint16_t soundId {0}; // the interned events (EntityManager::SoundName)
    std::uint16_t impactSoundId {0};
};

// The sounds of a unit or a building of its own (the `sounds:` block of its character file)
struct CharacterSounds
{
    std::string death;   // when it is destroyed (empty: an explosion / the infantry death / a wall coming down)
    std::string damage;  // when a bullet hits it (empty: the bullet hit / the armour hit); a gun's impact_sfx wins
    float volume {1.0f}; // times every sound of it (its guns, its hits, its death)
    std::uint16_t deathId {0};
    std::uint16_t damageId {0};
};

struct UnitConfig
{
    int archetypeId {0};
    std::string id;
    std::string name;
    UnitType type {UnitType::ResistanceRifleman};
    Faction faction {Faction::Resistance};
    std::string unitClass;
    int maxHp {100};
    float moveSpeed {1.4f};
    int sightRadius {5};
    int attackDamage {12};
    float attackRange {4.0f};
    float attackCooldown {0.7f};
    float attackCooldownMax {0.0f};
    int burstCount {1};
    float burstDelay {0.1f};
    float bulletSpeed {0.0f}; // combat.bullet_speed, px/s of rockets and shells (0: the projectile kind's own)
    int splashPercent {0};
    ProjectileType projectile {ProjectileType::Bullet};
    float muzzleOffsetX {0.0f}; // localePosition X
    float muzzleOffsetY {0.0f}; // localePosition Y
    float turretOffsetX {0.0f}; // turret_offset / dome_offset X
    float turretOffsetY {0.0f}; // turret_offset / dome_offset Y
    bool hasCustomTurretOffset {false};
    float spreadAngle {-1.0f};  // Spread angle in radians (-1 = unconfigured default, 0 = high precision pinpoint)
    float turnStepDelay {0.0f}; // Delay per 45-degree rotation step in seconds (0 for infantry)
    float turretTurnStepDelay {0.0f}; // Delay per 45-degree turret rotation step in seconds (0 for infantry)
    int creditCost {80};
    float buildTime {4.0f};
    std::string producedIn;
    bool isSiegeMode {false};
    int siegeArchetypeId {-1};
    bool siegeRequiresResearch {false}; // siege.requires_research: deploying needs the faction's siege technology
    // siege.auto_deploy: when the unit goes into siege mode by itself (the computer always, a person's units while on
    // an attack-move). Far: when an enemy is beyond the normal range but within the siege range (needs a longer
    // siege range); Always: whenever an enemy is within the siege range; Off: by hand only.
    enum class AutoSiege : std::uint8_t
    {
        Off,
        Far,
        Always
    };
    AutoSiege autoSiege {AutoSiege::Far};
    int parentArchetypeId {-1};
    bool hasRotatingTurret {false};
    std::string wreckType {"vehicle"}; // "infantry" or "vehicle"
    int wreckComposite {-1};           // sprite handle of the first wreck stage (-1: the vehicle wreck of effects.yaml)
    int wreckStages {5};
    // animations: block (timing of the pictures; the character editor's Animator edits it)
    float moveFrameTime {0.09f};   // seconds per walking frame
    float attackFrameTime {-1.0f}; // seconds per firing-pose frame (< 0: 0.07, or 0.04 for archetypes 7 and 18)
    float attackPoseTime {
        -1.0f};                  // how long the firing pose lasts after the first shot of a volley (< 0: by projectile)
    float burstPoseTime {0.15f}; // the same after every later shot of a burst
    int flashMode {0};   // muzzle flash layer: 0 loops while firing, 1 plays once per shot, 2 plays once and holds
    int flashFrames {0}; // frames the flash lasts (0 = its whole animation)
    std::vector<int> deathSequences;
    std::vector<int> flameDeathSequences;
    std::string description;
    std::vector<WeaponConfig> weapons;
    CharacterSounds sounds;
};

struct BuildingConfig
{
    int buildingIndex {0};
    std::string id;
    std::string name;
    UnitType type {UnitType::Headquarters};
    Faction faction {Faction::Resistance};
    int width {3};
    int height {3};
    int maxHp {1200};
    int sightRadius {6};
    int burstCount {1};       // shots of one volley (a Missile Turret fires 2 rockets)
    float burstDelay {0.1f};  // seconds between them
    float bulletSpeed {0.0f}; // combat.bullet_speed, px/s of rockets and shells (0: the projectile kind's own)
    int radarRange {0}; // stats.radar_range: tiles the finished building sees beyond its sight radius (Radar Locator)
    int creditCost {400};
    int powerCost {0}; // >0 produces, <0 consumes
    float buildTime {12.0f};
    int iconCompId {-1};
    bool canAttack {false};
    int attackDamage {0};
    float attackRange {0.0f};
    float attackCooldown {1.0f};
    float attackCooldownMax {0.0f};
    int wreckComposite {-1}; // animations.wreck_composite (sprite); -1 = rubble by footprint size
    int wreckStages {0};     // animations.wreck_stages; 0 = default (6)
    int splashPercent {0};
    ProjectileType projectile {ProjectileType::Bullet};
    float muzzleOffsetX {0.0f}; // localePosition X
    float muzzleOffsetY {0.0f}; // localePosition Y
    float turretOffsetX {0.0f}; // turret_offset / dome_offset X
    float turretOffsetY {0.0f}; // turret_offset / dome_offset Y
    bool hasCustomTurretOffset {false};
    float spreadAngle {-1.0f}; // Spread angle in radians (-1 = unconfigured default, 0 = high precision pinpoint)
    bool hasRotatingTurret {false};
    bool hasRotatingRadar {false};
    int buildRadius {16};
    std::string description;
    std::vector<WeaponConfig> weapons;
    CharacterSounds sounds;
};

class EntityManager
{
public:
    EntityManager();

    void Clear();
    void SetTextureManager(TextureManager *texManager)
    {
        m_texManager = texManager;
    }

    // `owner`: the player it belongs to; kAutoOwner = the (first) player of that faction (single player)
    static constexpr PlayerId kAutoOwner = 0xFE;
    int SpawnUnit(UnitType type, int tileX, int tileY, Faction faction);
    int SpawnUnitByArchetype(int archetypeId, int tileX, int tileY, Faction faction, PlayerId owner = kAutoOwner);
    // The unit becomes another form of itself (a Zeus gets its rocket turret): stats and looks of the new archetype,
    // health keeps its share, orders go on
    bool UpgradeUnitForm(int unitId, int newArchetype);
    int SpawnBuilding(UnitType type, int tileX, int tileY, Faction faction);
    // Replica worlds (TASKNEW.md step 3): makes the entities match the authority's snapshot without running any
    // behaviour: new ids are spawned under their own id, known ones get the sent state, a full snapshot kills the ones
    // it does not list. Returns false when the snapshot is not usable (an entity could not be created).
    bool ApplySnapshot(const WorldSnapshot &snapshot);
    // The visual-only part of Update (muzzle flashes, flame jets, explosions): also run by replicas
    void UpdateVisualEffects(float deltaTime);
    int SpawnBuildingByIndex(
        int buildingIdx,
        int tileX,
        int tileY,
        Faction faction,
        float initialProgress = 1.0f,
        PlayerId owner = kAutoOwner);

    bool LoadCharacterConfigs();
    // Parses one character YAML document (unit or building) and replaces its config; used by the
    // loader above and by the character editor to apply unsaved edits. Call rebuildBlueprints() after.
    // Taken by value: lookups of missing keys need a mutable node (pass YAML::Clone to keep yours intact).
    bool LoadCharacterYaml(YAML::Node root);
    // gettext key of a character's description: "<id>.description" (the text lives in po/*.po)
    static std::string DescriptionKey(std::string_view characterId);
    void RebuildBlueprints();
    const UnitConfig *GetUnitConfig(int archetypeId) const;
    const BuildingConfig *GetBuildingConfig(int buildingIndex) const;
    // The power a building of this kind gives (> 0) or takes (< 0): production.power of its character file
    int BuildingPower(int buildingIndex) const;
    const UnitConfig *GetUnitConfigByUnitType(UnitType type, Faction faction) const;

    // `player` builds there: within the radius of one of its headquarters, on ground it sees (people)
    // `checkBaseOpen` false: the (costly, whole-map) KeepsBaseOpen test of computer players is left to the caller,
    // which then asks KeepsBaseOpen itself for the few spots it really considers
    bool CanPlaceBuilding(
        int tileX,
        int tileY,
        int width,
        int height,
        const TileMap &map,
        PlayerId player,
        const char **reason = nullptr,
        bool checkBaseOpen = true) const;
    // The footprint would not shut the player's base in: from the headquarters and from the exits of the unit
    // producers the rest of the map stays reachable, and the new building keeps a free side of its own.
    // Checked for computer players (a human may wall himself in).
    bool KeepsBaseOpen(int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const;
    // Building radius around a player's headquarters (the mission's radius plus the player's technologies)
    int GetBuildRadius(PlayerId player) const;
    int BaseBuildRadius(PlayerId player) const; // radius of the player's faction before research
    int GetBuildRadius() const
    {
        return GetBuildRadius(Players::Local());
    }
    void SetBuildRadius(int r)
    {
        m_factionBuildRadius = {r, r};
        m_buildRadiusExtra = {};
    }
    void ExpandBuildRadius(int delta)
    {
        m_buildRadiusExtra[Players::Local() % kMaxPlayers] += delta;
    }
    // Technologies widen the base building radius by a percentage of the mission's own radius
    void SetBuildRadiusPercent(PlayerId player, int percent);
    // Build menu of the local player's faction
    static const std::vector<BuildingBlueprint> &GetBlueprints();
    // Build menu of any faction; blueprintId is the slot in the canonical order (HQ, power, barracks,
    // factory, lab, bunker, tower, wall), the same for both factions, so it is what a network command carries
    static const std::vector<BuildingBlueprint> &GetBlueprints(Faction faction);
    static const BuildingBlueprint *FindBlueprint(Faction faction, int blueprintId);

    bool HasBuildingType(UnitType type, PlayerId owner, bool mustBeComplete = true) const;
    int CountBuildingType(UnitType type, PlayerId owner, bool mustBeComplete = true) const;
    bool HasBuildingType(UnitType type, Faction faction, bool mustBeComplete = true) const
    {
        return HasBuildingType(type, Players::OwnerOf(faction), mustBeComplete);
    }
    std::vector<int> GetBuildingsByType(UnitType type, PlayerId owner, bool mustBeComplete = true) const;
    // The (first) player of that faction's buildings (single player, the AI)
    std::vector<int> GetBuildingsByType(UnitType type, Faction faction, bool mustBeComplete = true) const
    {
        return GetBuildingsByType(type, Players::OwnerOf(faction), mustBeComplete);
    }
    bool GetBuildingSpawnPoint(int buildingId, const TileMap &map, int &outTileX, int &outTileY) const;
    bool IsTileOccupiedByUnit(int tileX, int tileY, int ignoreEntityId = 0) const;

    void Update(float deltaTime, TileMap &map, Pathfinder &pathfinder);
    void UpdateFogOfWar(TileMap &map);
    // Something of the player took damage: an attacker hidden in the fog shows up for a moment
    // (only its own spot, so long-range units reveal where they fire from, not their surroundings)
    // `attackerId` hit something of `victim`: its team sees the attacker for a moment
    void NoteHit(PlayerId victim, int attackerId);
    // noteHit plus the victim remembers its attacker (retaliation, see Unit::updateCombat)
    void NoteEntityHit(Entity &victim, int attackerId);
    // Keeps Unit::turretShiftX/Y in line with the turret layer of the unit's current hull sprite
    void UpdateTurretShift(Unit &u) const;
    static void TurretShift(int archetypeId, int facingDirection, float &outShiftX, float &outShiftY);
    static inline void GetDeterministicTurretShift(
        int archetypeId, int facingDirection, float &outShiftX, float &outShiftY)
    {
        TurretShift(archetypeId, facingDirection, outShiftX, outShiftY);
    }
    // Siege-capable units deploy by themselves when a visible enemy is out of normal range but within
    // siege range (AI always, the player only on an attack-move [Q] order), and pack up again when idle
    void UpdateAutoSiege(Unit &u, const TileMap &map, float deltaTime);
    bool IsEntityVisibleToAi(const Entity &entity) const;
    // Seen by a computer player: within the sight radius of any of its units / buildings
    bool IsEntityVisibleToAi(const Entity &entity, PlayerId observer) const;
    // Seen by `observer`: own and allied things always; people see what their team's sight covers this
    // tick, the computer what is within the sight radius of its units / buildings
    bool IsEntityVisibleToPlayer(const Entity &entity, PlayerId observer, const TileMap &map) const;
    // An enemy structure as the local player may see it: while it is in sight, and afterwards (as it was) only when
    // it was seen before and is finished. What is built out of sight stays hidden, and so does its progress.
    bool IsHostileBuildingShown(const Building &building, const TileMap &map, bool *outInSight = nullptr) const;
    void RenderGroundRemnants(
        Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled = true) const;

    // Render interpolation. At the start of every simulation tick the positions of the units and shots are kept;
    // the picture then shows prev + (current - prev) * alpha, alpha being how far the frame is into the next tick
    // (0..1, set by the main loop). Nothing of it is part of the world.
    void SnapshotRenderState();
    void SetRenderAlpha(float alpha)
    {
        m_renderAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    }
    float RenderAlpha() const
    {
        return m_renderAlpha;
    }
    // Where the picture shows the unit (top-left world pixel, as GetWorldPixelX / Y)
    void RenderPosition(const Entity &entity, float &x, float &y) const;

    void Render(
        Renderer &renderer,
        const Camera &camera,
        const TileMap &map,
        bool fogEnabled = true,
        int globalAnimTick = 0,
        bool debugMode = false,
        bool showAllHealth = false);
    inline void render(
        Renderer &renderer,
        const Camera &camera,
        const TileMap &map,
        bool fogEnabled = true,
        int globalAnimTick = 0,
        bool debugMode = false,
        bool showAllHealth = false)
    {
        Render(renderer, camera, map, fogEnabled, globalAnimTick, debugMode, showAllHealth);
    }
    // What shines: projectiles and explosions (the glow pass of the shader effects draws them once more)
    void RenderEmissive(Renderer &renderer, const Camera &camera) const;
    // What lights up its surroundings at night (explosions, flames, rockets and shells in flight, buildings), in
    // world pixels: only those in sight when `fog` is on. How far and how bright: NightLighting (data/nights.json).
    // Drawing only
    struct LightSpot
    {
        enum class Kind : std::uint8_t
        {
            Explosion,
            FlameStart,  // where a flamethrower's jet leaves the gun
            FlameEnd,    // where it lands
            Projectile,  // a rocket or a shell in flight
            MuzzleFlash, // a shot leaving the gun (`age` in seconds)
            Building
        };
        Kind kind {Kind::Explosion};
        float x {0.0f}; // the centre (a building: the top left corner of its footprint)
        float y {0.0f};
        float w {0.0f}; // a building's footprint
        float h {0.0f};
        float age {0.0f};     // an explosion: 0 at its start .. 1 at its end
        bool powered {true};  // a building: has power
        bool complete {true}; // a building: finished
    };
    void CollectLights(const TileMap &map, bool fog, std::vector<LightSpot> &out) const;

    // Fortress auxiliary flamethrower, researched per faction
    bool IsFortressFlamethrowerUnlocked(PlayerId p) const
    {
        return p < kMaxPlayers && m_flamethrowerUnlocked[p];
    }
    // Siege deployment of units whose YAML says siege.requires_research
    bool IsSiegeUnlocked(PlayerId p) const
    {
        return p < kMaxPlayers && m_siegeUnlocked[p];
    }
    void SetSiegeUnlocked(PlayerId p, bool u)
    {
        if (p < kMaxPlayers)
            m_siegeUnlocked[p] = u;
    }
    bool CanDeploySiege(const Unit &u) const;
    void SetFortressFlamethrowerUnlocked(PlayerId p, bool u)
    {
        if (p < kMaxPlayers)
            m_flamethrowerUnlocked[p] = u;
    }

    // Sandbox (character editor battles): every structure counts as powered
    void SetUnlimitedPower(bool unlimited)
    {
        m_unlimitedPower = unlimited;
    }

    // A clicked ground tile as a destination: clamped into the map (a click at the very edge is a valid order) and,
    // when the tile cannot be stood on (impassable, wall), moved to the nearest tile that can (rings outward, ties by
    // scan order: deterministic)
    static void NearestOpenTile(const TileMap &map, int &tileX, int &tileY);
    void IssueMoveOrder(
        const std::vector<int> &unitIds,
        int targetTileX,
        int targetTileY,
        Pathfinder &pathfinder,
        bool attackMove = false);
    void IssueAttackMoveOrder(
        const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder);
    void IssueAttackOrder(const std::vector<int> &unitIds, int targetEntityId, Pathfinder *pathfinder = nullptr);
    void IssueAttackWallOrder(const std::vector<int> &unitIds, int wallTileX, int wallTileY, Pathfinder &pathfinder);
    void ToggleSiegeMode(const std::vector<int> &unitIds);

    // Selects the units of `owner` inside the box (and unselects the rest)
    void GetUnitsInBox(
        float minWorldX,
        float minWorldY,
        float maxWorldX,
        float maxWorldY,
        PlayerId owner,
        std::vector<int> &outUnitIds);
    int GetUnitAt(float worldX, float worldY, float tolerance = 15.0f) const;

    Entity *GetEntity(int id);
    const Entity *GetEntity(int id) const;

    Unit *GetUnit(int id);
    const Unit *GetUnit(int id) const;

    Building *GetBuilding(int id);
    const Building *GetBuilding(int id) const;

    const EntityContainer &GetAllEntities() const
    {
        return m_entities;
    }
    EntityContainer &GetAllEntities()
    {
        return m_entities;
    }

    const std::vector<Projectile> &GetProjectiles() const
    {
        return m_projectiles;
    }

    // Enemy units / buildings destroyed by a player (kill rewards)
    int GetKillCount(PlayerId player) const
    {
        return player < kMaxPlayers ? m_kills[player] : 0;
    }
    int GetPlayerKillCount() const
    {
        return GetKillCount(Players::Local());
    }
    // Match statistics (Tab): what a player destroyed of the enemy and lost itself, in credits of the
    // things' price; units and buildings counted separately
    struct PlayerStats
    {
        int unitsKilled {0};
        int buildingsDestroyed {0};
        int unitsLost {0};
        int buildingsLost {0};
        int valueDestroyed {0};
        int valueLost {0};
    };
    const PlayerStats &GetPlayerStats(PlayerId player) const
    {
        static const PlayerStats kNone {};
        return player < kMaxPlayers ? m_stats[player] : kNone;
    }
    // Efficiency points: a point per 10 credits of enemy forces destroyed
    int GetEfficiencyPoints(PlayerId player) const
    {
        return GetPlayerStats(player).valueDestroyed / 10;
    }
    // Sounds of the world, collected for the game's sound effects (only a picture: never part of the simulation).
    // Deaths are found by the sound side itself (entities that turned to Die).
    enum class SoundCue : std::uint8_t
    {
        FireRifle,
        FireMg,
        FireSniper,
        FireCannon,
        FireCannonHigh, // the heavy tanks: Zeus and Rhino
        FireRocket,
        FireMlrs,
        FireFlame,
        HitBullet,    // a bullet into a soldier
        HitArmor,     // a bullet into a vehicle, building or wall
        ExplodeSmall, // a shell / rocket lands
        WallDestroyed
    };
    struct SoundCueEvent
    {
        SoundCue cue {SoundCue::FireRifle};
        float x {0.0f};
        float y {0.0f};
        PlayerId owner {kNoPlayer}; // who fired
        std::uint16_t sound {0};    // the event of the character / weapon (SoundName), 0: the cue's own
        float gain {1.0f};          // the volume of the character / weapon
    };
    // The sound events named by the character files (0: none)
    const std::string &SoundName(std::uint16_t id) const
    {
        static const std::string kNone;
        return id > 0 && id <= m_soundNames.size() ? m_soundNames[id - 1] : kNone;
    }
    // The death sound of a unit / building and the volume of its sounds
    const CharacterSounds *SoundsOf(const Entity &e) const;
    // The sounds of the wall segments of a side (the wall character of its faction; null: none loaded)
    const CharacterSounds *WallSoundsOf(Faction faction) const;
    // Changes whenever the world is cleared (entity ids start again)
    std::uint32_t Generation() const
    {
        return m_generation;
    }
    // Buildings that blew up since the last call (visual only: the camera shake)
    struct BuildingBlast
    {
        float x {0.0f};
        float y {0.0f};
        float size {1.0f}; // ~1 for a medium building
    };
    std::vector<BuildingBlast> TakeBuildingBlasts()
    {
        std::vector<BuildingBlast> out;
        out.swap(m_buildingBlasts);
        return out;
    }
    // What a replica needs besides the entities (TASKNEW.md step 5): the sounds, building blasts and explosions of the
    // ticks since the last call, collected on the authority only while a network match runs in authority mode
    struct ReplicaExplosion
    {
        float x {0.0f};
        float y {0.0f};
        int anim {0};
        float delay {0.0f};
    };
    std::unordered_set<int> &CollectedDeaths()
    {
        return m_collectedDeaths;
    }
    struct ReplicaGroundMark
    {
        float x, y;
        int composite;
        float seconds;
    };
    struct ReplicaEvents
    {
        std::vector<SoundCueEvent> cues;
        std::vector<BuildingBlast> blasts;
        std::vector<ReplicaExplosion> explosions;
        std::vector<ReplicaGroundMark> groundMarks;
    };
    void SetCollectReplicaEvents(bool on)
    {
        m_collectEvents = on;
    }
    ReplicaEvents TakeReplicaEvents()
    {
        ReplicaEvents out;
        out.cues.swap(m_eventCues);
        out.blasts.swap(m_eventBlasts);
        out.explosions.swap(m_eventExplosions);
        out.groundMarks.swap(m_eventGroundMarks);
        return out;
    }
    std::vector<SoundCueEvent> TakeSoundCues()
    {
        std::vector<SoundCueEvent> out;
        out.swap(m_soundCues);
        return out;
    }

    // The player left a network match: every building, wall and unit of it is destroyed at once
    // (no kills for anybody)
    void DestroyPlayer(PlayerId player, TileMap &map);
    // A match: a player who has no building left (under construction counts; units and walls do not) is out. His
    // forces are destroyed as when a player leaves. Returns who dropped out on this call. Depends on the state of
    // the world only, so every peer does it on the same tick.
    std::vector<PlayerId> UpdateElimination(TileMap &map);
    // Override the default match limit for self-contained scenes with larger starting armies.
    void SetEntityLimit(int limit)
    {
        m_entityLimit = limit > 0 ? limit : MAX_UNITS;
    }
    // How many more entities the player may get (its even share in a match of 3+ players).
    int FreeEntitySlots(PlayerId owner) const;
    bool IsEliminated(PlayerId player) const
    {
        return player < kMaxPlayers && m_eliminated[player];
    }
    // Price of a unit / building in credits (statistics)
    int ValueOf(const Entity &e) const;
    void UpdateRegeneration(float deltaTime);

    static int ApplyMovingUnitDamage(int baseDamage, const Entity *target, ProjectileType kind);

    int GetUnitIdAtTile(int tileX, int tileY, int ignoreEntityId = 0) const;
    void ResolveUnitStacking(TileMap &map);

    // The owner blows up its own structure (sidebar "Destroy"); returns the credits of the orders that were
    // still queued in it, for a refund
    int DemolishBuilding(int buildingId, TileMap &map);

    // ---- Mines (assets/characters/mines.yaml): laid on one tile, invisible to the other side, fired by hostile units
    struct Mine
    {
        int id {0};
        PlayerId owner {kNoPlayer};
        int tileX {0};
        int tileY {0};
        int kind {0};         // index into Mines::Specs()
        float armLeft {0.0f}; // seconds until it can fire
    };
    const std::vector<Mine> &GetMines() const
    {
        return m_mines;
    }
    int CountMines(PlayerId owner, int kind) const;
    // `player` may lay a mine of that kind there (the rules of a wall segment, a free tile, the kind's own limit)
    bool CanPlaceMine(int kind, int tileX, int tileY, const TileMap &map, PlayerId player) const;
    // Lays it (the credits are the caller's); false when it may not be laid there
    bool PlaceMine(int kind, int tileX, int tileY, const TileMap &map, PlayerId player);
    void UpdateMines(float deltaTime, TileMap &map);
    // The mines the local player's side sees (its own and its allies')
    void RenderMines(Renderer &renderer, const Camera &camera, const TileMap &map, PlayerId viewer) const;

    void SpawnExplosion(float worldX, float worldY, int animId = 12, float delay = 0.0f);
    // Destruction of a building or vehicle: the main blast plus secondary ones (over a building's whole footprint)
    void SpawnDeathExplosions(const Entity &e);
    void SpawnProjectileFromEntity(
        Entity &entity, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map);

private:
    int m_nextId {1};
    std::array<int, kMaxPlayers> m_kills {};
    std::array<PlayerStats, kMaxPlayers> m_stats {};
    void NoteKill(PlayerId victim, PlayerId killer)
    {
        if (killer < kMaxPlayers && Players::Hostile(victim, killer))
            ++m_kills[killer];
    }
    // A kill with the statistics of both sides
    void NoteKill(const Entity &victim, PlayerId killer);
    // Only the statistics (kills by splash damage, which earn no kill reward)
    void NoteStats(const Entity &victim, PlayerId killer);
    // Puts a living entity into its death (state, death animation, explosions, frees a building's tiles)
    void KillEntity(Entity &e, TileMap &map);
    std::array<int, 2> m_factionBuildRadius {16, 16};   // build_radius of each faction's headquarters, before research
    std::array<bool, kMaxPlayers> m_eliminated {};      // players who have lost every building (UpdateElimination)
    std::array<int, kMaxPlayers> m_buildRadiusExtra {}; // tiles each player's technologies add
    bool m_flamethrowerUnlocked[kMaxPlayers] {};
    mutable std::unordered_set<std::uint64_t>
        m_seenBuildings; // enemy structures the local player has seen (see isHostileBuildingShown)
    bool m_siegeUnlocked[kMaxPlayers] {};

    struct RevealPing
    {
        int entityId;      // the attacker: its footprint is revealed for as long as the ping lasts
        PlayerId observer; // whose thing it hit (that player's team sees it)
        float timeLeft;
    };
    std::vector<RevealPing> m_revealPings; // attackers spotted by their muzzle flash
    bool m_unlimitedPower {false};
    int m_entityLimit {MAX_UNITS};
    EntityContainer m_entities;
    std::vector<Projectile> m_projectiles;
    float m_renderAlpha {1.0f}; // render interpolation between the last two ticks (see SnapshotRenderState)
    std::vector<ExplosionParticle> m_explosions;
    std::vector<SmokePuff> m_smokePuffs;
    std::vector<FlameBurst> m_flameBursts;
    // Muzzle flashes of the last moments, for the night's light (visual only: never part of the world's checksum)
    struct MuzzleFlash
    {
        float x {0.0f};
        float y {0.0f};
        float age {0.0f}; // seconds
    };
    std::vector<MuzzleFlash> m_muzzleFlashes;
    static constexpr float kMuzzleFlashKeep = 0.5f; // longest a flash's light may last (data/nights.json)
    // Small craters where shells and rockets landed (only a picture: not part of the simulation state)
    struct GroundMark
    {
        float x {0.0f};
        float y {0.0f};
        int composite {0};
        float timeLeft {0.0f};
    };
    std::vector<GroundMark> m_groundMarks;
    std::vector<Mine> m_mines; // part of the simulation state (Simulation::VisitState)
    int m_nextMineId {1};
    std::vector<SoundCueEvent> m_soundCues;
    std::vector<BuildingBlast> m_buildingBlasts;
    bool m_collectEvents {false};
    std::unordered_set<int> m_collectedDeaths;
    std::unordered_set<int> m_replicaVisible;
    bool m_replicaWorld {false};
    bool m_replicaApplying {false}; // ApplySnapshot is spawning: no entity limit
    std::vector<SoundCueEvent> m_eventCues;
    std::vector<BuildingBlast> m_eventBlasts;
    std::vector<ReplicaExplosion> m_eventExplosions;
    std::vector<ReplicaGroundMark> m_eventGroundMarks;
    std::uint32_t m_generation {0};
    void Cue(SoundCue c, float x, float y, PlayerId owner, std::uint16_t sound = 0, float gain = 1.0f)
    {
        if (m_soundCues.size() < 256) // nobody takes them (headless, editor): they stop piling up
            m_soundCues.push_back({c, x, y, owner, sound, gain});
        if (m_collectEvents && m_eventCues.size() < 512)
            m_eventCues.push_back({c, x, y, owner, sound, gain});
    }
    std::vector<std::string> m_soundNames; // only grows: projectiles in flight keep their ids over a reload
    std::uint16_t InternSound(std::string_view name);
    // The gun firing now: weapons[1] while a unit's second gun shoots, else the main one (null: none in the file)
    const WeaponConfig *FiringWeapon(const Entity &e) const;
    void AddGroundMark(float x, float y, int composite, float seconds);

public:
    // Flamethrower jets on screen (tests)
    std::size_t FlameBurstCount() const
    {
        return m_flameBursts.size();
    }
    // Craters on the ground (tests)
    std::size_t GroundMarkCount() const
    {
        return m_groundMarks.size();
    }

private:
    TextureManager *m_texManager {nullptr};
    std::unordered_map<int, UnitConfig> m_unitConfigs;
    std::unordered_map<int, BuildingConfig> m_buildingConfigs;
    static std::vector<BuildingBlueprint> s_blueprints;
    static std::vector<BuildingBlueprint> s_factionBlueprints[2]; // [0] Confederation, [1] Resistance
};

} // namespace OpenArtOfWar
