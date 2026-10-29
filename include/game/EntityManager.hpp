#pragma once

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

namespace YAML
{
    class Node;
}

namespace OpenArtOfWar
{

    class TileMap;
    class TextureManager;

    /**
     * @brief Container adapter for polymorphic game entities, providing
     * transparent iteration (yielding Entity&) and index access.
     */
    class EntityContainer
    {
    public:
        using Storage = std::vector<std::unique_ptr<Entity>>;

        size_t size() const
        {
            return m_items.size();
        }
        bool empty() const
        {
            return m_items.empty();
        }
        void reserve(size_t n)
        {
            m_items.reserve(n);
        }
        void clear()
        {
            m_items.clear();
        }

        Entity &operator[](size_t i)
        {
            return *m_items[i];
        }
        const Entity &operator[](size_t i) const
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
            explicit iterator(Storage::iterator it) : m_it(it)
            {
            }

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
            explicit const_iterator(Storage::const_iterator it) : m_it(it)
            {
            }

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

        Storage &raw()
        {
            return m_items;
        }
        const Storage &raw() const
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
        int iconCompId {2074}; // Composite sprite ID on sheet_16.png (2064..2081)
        UnitType unitType {UnitType::Headquarters};
        std::string name;
        int width {3};
        int height {3};
        int creditCost {400};
        int powerCost {0}; // >0 produces, <0 consumes
        std::string desc;
        bool isWall {false}; // placed as a 1x1 wall segment instead of a building
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
        int splashPercent {0};
        ProjectileType projectile {ProjectileType::Bullet};
        float muzzleOffsetX {0.0f}; // localePosition X
        float muzzleOffsetY {0.0f}; // localePosition Y
        float turretOffsetX {0.0f}; // turret_offset / dome_offset X
        float turretOffsetY {0.0f}; // turret_offset / dome_offset Y
        bool hasCustomTurretOffset {false};
        float spreadAngle {-1.0f};  // Spread angle in radians (-1 = unconfigured default, 0 = high precision pinpoint)
        float turnStepDelay {0.0f}; // Delay per 45-degree rotation step in seconds (0 for infantry)
        int creditCost {80};
        float buildTime {4.0f};
        std::string producedIn;
        bool isSiegeMode {false};
        int siegeArchetypeId {-1};
        bool siegeRequiresResearch {false}; // siege.requires_research: deploying needs the faction's siege technology
        int parentArchetypeId {-1};
        bool hasRotatingTurret {false};
        std::string wreckType {"vehicle"}; // "infantry" or "vehicle"
        int wreckComposite {1764};
        int wreckStages {5};
        std::vector<int> deathSequences;
        std::vector<int> flameDeathSequences;
        std::string description;
        std::vector<WeaponConfig> weapons;
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
        int creditCost {400};
        int powerCost {0}; // >0 produces, <0 consumes
        float buildTime {12.0f};
        int iconCompId {2074};
        bool canAttack {false};
        int attackDamage {0};
        float attackRange {0.0f};
        float attackCooldown {1.0f};
        float attackCooldownMax {0.0f};
        int wreckComposite {0}; // animations.wreck_composite; 0 = rubble by footprint size
        int wreckStages {0};    // animations.wreck_stages; 0 = default (6)
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
    };

    class EntityManager
    {
    public:
        EntityManager();

        void clear();
        void setTextureManager(TextureManager *texManager)
        {
            m_texManager = texManager;
        }

        // `owner`: the player it belongs to; kAutoOwner = the (first) player of that faction (single player)
        static constexpr PlayerId kAutoOwner = 0xFE;
        int spawnUnit(UnitType type, int tileX, int tileY, Faction faction);
        int spawnUnitByArchetype(int archetypeId, int tileX, int tileY, Faction faction, PlayerId owner = kAutoOwner);
        int spawnBuilding(UnitType type, int tileX, int tileY, Faction faction);
        int spawnBuildingByIndex(int buildingIdx, int tileX, int tileY, Faction faction, float initialProgress = 1.0f, PlayerId owner = kAutoOwner);

        bool loadCharacterConfigs(const std::string &assetRoot = "assets");
        // Parses one character YAML document (unit or building) and replaces its config; used by the
        // loader above and by the character editor to apply unsaved edits. Call rebuildBlueprints() after.
        // Taken by value: lookups of missing keys need a mutable node (pass YAML::Clone to keep yours intact).
        bool loadCharacterYaml(YAML::Node root);
        // gettext key of a character's description: "<id>.description" (the text lives in po/*.po)
        static std::string descriptionKey(const std::string &characterId);
        void rebuildBlueprints();
        const UnitConfig *getUnitConfig(int archetypeId) const;
        const BuildingConfig *getBuildingConfig(int buildingIndex) const;
        const UnitConfig *getUnitConfigByUnitType(UnitType type, Faction faction) const;

        // `player` builds there: within the radius of one of its headquarters, on ground it sees (people)
        bool canPlaceBuilding(int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const;
        // The footprint would not shut the player's base in: from the headquarters and from the exits of the unit
        // producers the rest of the map stays reachable, and the new building keeps a free side of its own.
        // Checked for computer players (a human may wall himself in).
        bool keepsBaseOpen(int tileX, int tileY, int width, int height, const TileMap &map, PlayerId player) const;
        // Building radius around a player's headquarters (the mission's radius plus the player's technologies)
        int getBuildRadius(PlayerId player) const;
        int getBuildRadius() const
        {
            return getBuildRadius(Players::local());
        }
        void setBuildRadius(int r)
        {
            m_baseBuildRadius = r;
            m_buildRadiusExtra = {};
        }
        void expandBuildRadius(int delta)
        {
            m_buildRadiusExtra[Players::local() % kMaxPlayers] += delta;
        }
        // Technologies widen the base building radius by a percentage of the mission's own radius (people only:
        // the computer's bases are laid out by the AI)
        void setBuildRadiusPercent(PlayerId player, int percent);
        static const std::vector<BuildingBlueprint> &getBlueprints();

        bool hasBuildingType(UnitType type, PlayerId owner, bool mustBeComplete = true) const;
        bool hasBuildingType(UnitType type, Faction faction, bool mustBeComplete = true) const
        {
            return hasBuildingType(type, Players::ownerOf(faction), mustBeComplete);
        }
        std::vector<int> getBuildingsByType(UnitType type, PlayerId owner, bool mustBeComplete = true) const;
        // The (first) player of that faction's buildings (single player, the AI)
        std::vector<int> getBuildingsByType(UnitType type, Faction faction, bool mustBeComplete = true) const
        {
            return getBuildingsByType(type, Players::ownerOf(faction), mustBeComplete);
        }
        bool getBuildingSpawnPoint(int buildingId, const TileMap &map, int &outTileX, int &outTileY) const;
        bool isTileOccupiedByUnit(int tileX, int tileY, int ignoreEntityId = 0) const;

        void update(float deltaTime, TileMap &map, Pathfinder &pathfinder);
        void updateFogOfWar(TileMap &map);
        // Something of the player took damage: an attacker hidden in the fog shows up for a moment
        // (only its own spot, so long-range units reveal where they fire from, not their surroundings)
        // `attackerId` hit something of `victim`: its team sees the attacker for a moment
        void noteHit(PlayerId victim, int attackerId);
        // Keeps Unit::turretShiftX/Y in line with the turret layer of the unit's current hull sprite
        void updateTurretShift(Unit &u) const;
        // Siege-capable units deploy by themselves when a visible enemy is out of normal range but within
        // siege range (AI always, the player only on an attack-move [Q] order), and pack up again when idle
        void updateAutoSiege(Unit &u, const TileMap &map, float deltaTime);
        bool isEntityVisibleToAi(const Entity &entity) const;
        // Seen by a computer player: within the sight radius of any of its units / buildings
        bool isEntityVisibleToAi(const Entity &entity, PlayerId observer) const;
        // Seen by `observer`: own and allied things always; people see what their team's sight covers this
        // tick, the computer what is within the sight radius of its units / buildings
        bool isEntityVisibleToPlayer(const Entity &entity, PlayerId observer, const TileMap &map) const;
        // An enemy structure as the local player may see it: while it is in sight, and afterwards (as it was) only when
        // it was seen before and is finished. What is built out of sight stays hidden, and so does its progress.
        bool isHostileBuildingShown(const Building &building, const TileMap &map, bool *outInSight = nullptr) const;
        void renderGroundRemnants(Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled = true) const;
        void render(Renderer &renderer, const Camera &camera, const TileMap &map, bool fogEnabled = true, int globalAnimTick = 0, bool debugMode = false, bool showAllHealth = false);
        // What shines: projectiles and explosions (the glow pass of the shader effects draws them once more)
        void renderEmissive(Renderer &renderer, const Camera &camera) const;

        // Fortress auxiliary flamethrower, researched per faction
        bool isFortressFlamethrowerUnlocked(PlayerId p) const
        {
            return p < kMaxPlayers && m_flamethrowerUnlocked[p];
        }
        // Siege deployment of units whose YAML says siege.requires_research
        bool isSiegeUnlocked(PlayerId p) const
        {
            return p < kMaxPlayers && m_siegeUnlocked[p];
        }
        void setSiegeUnlocked(PlayerId p, bool u)
        {
            if(p < kMaxPlayers)
                m_siegeUnlocked[p] = u;
        }
        bool canDeploySiege(const Unit &u) const;
        void setFortressFlamethrowerUnlocked(PlayerId p, bool u)
        {
            if(p < kMaxPlayers)
                m_flamethrowerUnlocked[p] = u;
        }

        // Sandbox (character editor battles): every structure counts as powered
        void setUnlimitedPower(bool unlimited)
        {
            m_unlimitedPower = unlimited;
        }

        void issueMoveOrder(const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder, bool attackMove = false);
        void issueAttackMoveOrder(const std::vector<int> &unitIds, int targetTileX, int targetTileY, Pathfinder &pathfinder);
        void issueAttackOrder(const std::vector<int> &unitIds, int targetEntityId, Pathfinder *pathfinder = nullptr);
        void issueAttackWallOrder(const std::vector<int> &unitIds, int wallTileX, int wallTileY, Pathfinder &pathfinder);
        void toggleSiegeMode(const std::vector<int> &unitIds);

        // Selects the units of `owner` inside the box (and unselects the rest)
        void getUnitsInBox(float minWorldX, float minWorldY, float maxWorldX, float maxWorldY, PlayerId owner, std::vector<int> &outUnitIds);
        int getUnitAt(float worldX, float worldY, float tolerance = 15.0f) const;

        Entity *getEntity(int id);
        const Entity *getEntity(int id) const;

        Unit *getUnit(int id);
        const Unit *getUnit(int id) const;

        Building *getBuilding(int id);
        const Building *getBuilding(int id) const;

        const EntityContainer &getAllEntities() const
        {
            return m_entities;
        }
        EntityContainer &getAllEntities()
        {
            return m_entities;
        }

        const std::vector<Projectile> &getProjectiles() const
        {
            return m_projectiles;
        }

        // Enemy units / buildings destroyed by a player (kill rewards)
        int getKillCount(PlayerId player) const
        {
            return player < kMaxPlayers ? m_kills[player] : 0;
        }
        int getPlayerKillCount() const
        {
            return getKillCount(Players::local());
        }
        void updateRegeneration(float deltaTime);

        static int applyMovingUnitDamage(int baseDamage, const Entity *target);

        int getUnitIdAtTile(int tileX, int tileY, int ignoreEntityId = 0) const;
        void resolveUnitStacking(TileMap &map);

        // The owner blows up its own structure (sidebar "Destroy"); returns the credits of the orders that were
        // still queued in it, for a refund
        int demolishBuilding(int buildingId, TileMap &map);

        void spawnExplosion(float worldX, float worldY, int animId = 12, float delay = 0.0f);
        // Destruction of a building or vehicle: the main blast plus secondary ones (over a building's whole footprint)
        void spawnDeathExplosions(const Entity &e);
        void spawnProjectileFromEntity(Entity &entity, float srcX, float srcY, float dstX, float dstY, int targetEntityId, TileMap &map);

    private:
        int m_nextId {1};
        std::array<int, kMaxPlayers> m_kills {};
        void noteKill(PlayerId victim, PlayerId killer)
        {
            if(killer < kMaxPlayers && Players::hostile(victim, killer))
                ++m_kills[killer];
        }
        int m_baseBuildRadius {16};                         // radius before Base Expansion research
        std::array<int, kMaxPlayers> m_buildRadiusExtra {}; // tiles each player's technologies add
        bool m_flamethrowerUnlocked[kMaxPlayers] {};
        mutable std::unordered_set<uint64_t> m_seenBuildings; // enemy structures the local player has seen (see isHostileBuildingShown)
        bool m_siegeUnlocked[kMaxPlayers] {};

        struct RevealPing
        {
            int entityId;      // the attacker: its footprint is revealed for as long as the ping lasts
            PlayerId observer; // whose thing it hit (that player's team sees it)
            float timeLeft;
        };
        std::vector<RevealPing> m_revealPings; // attackers spotted by their muzzle flash
        bool m_unlimitedPower {false};
        EntityContainer m_entities;
        std::vector<Projectile> m_projectiles;
        std::vector<ExplosionParticle> m_explosions;
        std::vector<SmokePuff> m_smokePuffs;
        TextureManager *m_texManager {nullptr};
        std::unordered_map<int, UnitConfig> m_unitConfigs;
        std::unordered_map<int, BuildingConfig> m_buildingConfigs;
        static std::vector<BuildingBlueprint> s_blueprints;
    };

} // namespace OpenArtOfWar
