#pragma once

#include "core/Config.hpp"
#include "game/Players.hpp"
#include <string>
#include <memory>
#include <vector>

namespace OpenArtOfWar
{

    class Unit;
    class Building;
    class TileMap;
    class Pathfinder;
    class Renderer;
    class Camera;

    enum class ProjectileType
    {
        Bullet = 0,
        Shell = 1,
        Rocket = 2,
        Flame = 3
    };

    struct SmokePuff
    {
        float x {0.0f};
        float y {0.0f};
        float lifetime {0.0f};
        float maxLifetime {0.35f};
        bool active {true};
    };

    struct Projectile
    {
        float x {0.0f};
        float y {0.0f};
        float startX {0.0f};
        float startY {0.0f};
        float targetX {0.0f};
        float targetY {0.0f};
        float speed {12.0f};
        int damage {10};
        int splashPercent {0};
        Faction faction {Faction::Resistance};
        PlayerId owner {0}; // who fired: no damage to them or their allies
        int targetId {0};
        int shooterId {0}; // who fired (revealed in the fog when it hits the player)
        ProjectileType type {ProjectileType::Bullet};
        int direction {0}; // 0..15 (for 16-dir rockets 1405+)
        int dir32 {0};     // 0..31 (for 32-dir bullets 1277+ and shells 1341+)
        bool active {false};
        float smokeTimer {0.0f};
    };

    struct ExplosionParticle
    {
        float x {0.0f};
        float y {0.0f};
        int animId {12};
        int currentFrame {0};
        int totalFrames {8};
        float frameTimer {0.0f};
        float delay {0.0f}; // seconds before it starts (chains of explosions over a big building)
        bool active {false};
    };

    /**
     * @brief Base class for all game world entities (units and buildings).
     */
    class Entity
    {
    public:
        int id {0};
        UnitType type {UnitType::ResistanceRifleman};
        Faction faction {Faction::Resistance}; // what it is: units, technologies, sprites of that side
        PlayerId owner {0};                    // whose it is: Players decides who is friend or foe

        // Position in world space (tileX * 30 + subX)
        int tileX {0};
        int tileY {0};
        // Signed fine pixel offset from the ORIGIN tile toward the tile being entered.
        // tileX/tileY only change when a step is fully completed (no half-way tile flips).
        float subX {0.0f};
        float subY {0.0f};

        // Tile reservation: the adjacent tile a unit is currently stepping into.
        // While a step is in progress the unit owns BOTH tileX/tileY and resTileX/resTileY,
        // so no other unit can enter either tile (prevents stacking and "squeezing" into each other).
        int resTileX {-1};
        int resTileY {-1};
        bool hasReservation() const noexcept
        {
            return resTileX >= 0 && resTileY >= 0;
        }
        void releaseReservation() noexcept
        {
            resTileX = -1;
            resTileY = -1;
        }
        bool occupiesTile(int x, int y) const noexcept
        {
            return (tileX == x && tileY == y) || (resTileX == x && resTileY == y);
        }

        // Health
        int currentHp {100};
        int maxHp {100};

        // Common combat attributes
        int targetEntityId {0};
        int targetWallTileX {-1};
        int targetWallTileY {-1};
        int attackDamage {0};
        float attackRange {0.0f};
        float attackCooldown {1.0f};
        float attackCooldownMax {0.0f}; // > attackCooldown: each cycle waits a random time in between

        // Delay until the next attack cycle (fixed, or random within [attackCooldown, attackCooldownMax])
        float nextCooldown() const;
        float cooldownTimer {0.0f};
        float attackAnimTimer {0.0f};
        float deathTimer {0.0f};
        int turretFacing {0};
        ProjectileType projectile {ProjectileType::Bullet};
        int splashPercent {0};
        float spreadAngle {-1.0f}; // Weapon dispersion angle in radians (-1 = unconfigured default, 0 = high precision pinpoint)

        // Sight & Fog
        int sightRadius {6}; // tiles

        // State & Selection
        ActionState state {ActionState::Idle};
        bool alive {true};
        bool selected {false};
        bool isBuilding {false}; // Fast check & backward compatibility
        bool hasRotatingTurret {false};

        // Animation
        int animFrame {0};
        float animTimer {0.0f};

        explicit Entity(bool isBuildingVal = false);
        virtual ~Entity() = default;

        virtual bool isBuildingEntity() const
        {
            return isBuilding;
        }
        virtual bool isUnitEntity() const
        {
            return !isBuilding;
        }

        virtual Unit *asUnit()
        {
            return nullptr;
        }
        virtual const Unit *asUnit() const
        {
            return nullptr;
        }

        virtual Building *asBuilding()
        {
            return nullptr;
        }
        virtual const Building *asBuilding() const
        {
            return nullptr;
        }

        float getWorldPixelX() const
        {
            return tileX * TILE_WIDTH + subX;
        }
        float getWorldPixelY() const
        {
            return tileY * TILE_HEIGHT + subY;
        }
        virtual float getUnitCenterPixelX() const
        {
            return getWorldPixelX() + (TILE_WIDTH * 0.5f);
        }
        virtual float getUnitCenterPixelY() const
        {
            return getWorldPixelY() + (TILE_HEIGHT * 0.5f);
        }

        virtual void getMuzzlePoint(float &outX, float &outY) const;
    };

} // namespace OpenArtOfWar
