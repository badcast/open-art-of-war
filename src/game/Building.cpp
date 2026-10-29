#include "game/Building.hpp"
#include "core/DetMath.hpp"
#include "core/Utility.hpp"
#include "game/Effects.hpp"
#include "game/TileMap.hpp"
#include <cmath>
#include <algorithm>

namespace OpenArtOfWar
{

    Building::Building() : Entity(true)
    {
        maxHp = 1200;
        currentHp = 1200;
        sightRadius = 7;
    }

    int Building::getPowerOutput() const
    {
        int bMod = (rawBuildingType >= 0) ? (rawBuildingType % 8) : 0;
        if(bMod == 0)
            return 50; // HQ
        if(bMod == 1)
            return 100; // Power Plant
        if(bMod == 2)
            return -20; // Barracks
        if(bMod == 3)
            return -40; // Factory
        if(bMod == 4)
            return -30; // Research Lab
        if(rawBuildingType == 5 || rawBuildingType == 13)
            return 0; // Concrete Bunkers (0 kW, autonomous pillbox)
        return -15;   // Turrets / Radar
    }

    void Building::getMuzzlePoint(float &outX, float &outY) const
    {
        float bcx = (tileX + buildWidth * 0.5f) * TILE_WIDTH;
        float bcy = (tileY + buildHeight * 0.5f) * TILE_HEIGHT;

        const float *s_dirDx = Utility::kDir8X;
        const float *s_dirDy = Utility::kDir8Y;

        if(hasRotatingTurret)
        {
            // Physical turret pivot center (Building 7 Missile Turret is elevated 16px above foundation)
            float tOx = 0.0f, tOy = -16.0f;
            if(hasCustomTurretOffset)
            {
                tOx = turretOffsetX;
                tOy = turretOffsetY;
            }

            float turretPivotX = bcx + tOx;
            float turretPivotY = bcy + tOy;

            int tf = (turretFacing % 8 + 8) % 8;
            outX = turretPivotX + s_dirDx[tf] * muzzleOffsetX - s_dirDy[tf] * muzzleOffsetY;
            outY = turretPivotY + s_dirDy[tf] * muzzleOffsetX + s_dirDx[tf] * muzzleOffsetY;
        }
        else
        {
            // Non-turret buildings (Bunker, Guard Tower, etc.): local offset from building center
            outX = bcx + muzzleOffsetX;
            outY = bcy + muzzleOffsetY;
        }
    }

    void Building::update(float deltaTime, TileMap &map, const BuildingDelegates &delegates)
    {
        if(!alive)
            return;

        if(state == ActionState::Die)
        {
            deathTimer -= deltaTime;
            const EffectsConfig &fx = EffectsConfig::get();
            const float elapsed = std::clamp(fx.burialSeconds - deathTimer, 0.0f, fx.burialSeconds);
            const int stages = wreckStages > 0 ? wreckStages : fx.wreckOfBuilding(buildWidth, buildHeight).stages;
            animFrame = std::min(stages - 1, static_cast<int>((elapsed / fx.burialSeconds) * static_cast<float>(stages)));
            if(deathTimer <= 0.0f)
            {
                alive = false;
            }
            return;
        }

        if(cooldownTimer > 0.0f)
        {
            cooldownTimer -= deltaTime;
        }

        if(attackAnimTimer > 0.0f)
        {
            attackAnimTimer -= deltaTime;
            if(attackAnimTimer <= 0.0f && state == ActionState::Attack)
            {
                state = ActionState::Idle;
            }
        }

        updateConstruction(deltaTime);
        updateProduction(deltaTime);
        updateDefenseCombat(deltaTime, map, delegates);
    }

    void Building::updateConstruction(float deltaTime)
    {
        if(buildProgress < 1.0f)
        {
            float dProg = deltaTime * (1.0f / 12.0f);
            buildProgress += dProg;
            float hpGain = maxHp * 0.90f * dProg;
            currentHp += static_cast<int>(std::round(hpGain));
            if(buildProgress >= 1.0f)
            {
                buildProgress = 1.0f;
                currentHp = std::min(currentHp, maxHp);
            }
        }
    }

    void Building::updateProduction(float deltaTime)
    {
        if(productionTimer > 0.0f)
        {
            productionTimer -= deltaTime;
            if(productionTimer <= 0.0f)
            {
                productionTimer = 0.0f;
                isProducing = false;
            }
            else
            {
                isProducing = true;
            }
        }
    }

    void Building::updateDefenseCombat(float /*deltaTime*/, TileMap &map, const BuildingDelegates &delegates)
    {
        bool requiresPower = (rawBuildingType != 5 && rawBuildingType != 13);
        if(attackDamage <= 0 || buildProgress < 1.0f || (requiresPower && !isPowered) || state == ActionState::Die)
        {
            return;
        }

        float srcX = (tileX + buildWidth * 0.5f) * TILE_WIDTH;
        float srcY = (tileY + buildHeight * 0.5f) * TILE_HEIGHT;

        // Validate current target
        if(targetEntityId != 0)
        {
            Entity *tgt = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
            if(!tgt || !tgt->alive || tgt->state == ActionState::Die || !Players::hostile(tgt->owner, owner) || (delegates.isEntityVisible && !delegates.isEntityVisible(*tgt, owner)))
            {
                targetEntityId = 0;
            }
            else
            {
                float dstX = tgt->getUnitCenterPixelX();
                float dstY = tgt->getUnitCenterPixelY();
                float dist = DetMath::hypot(dstX - srcX, dstY - srcY);
                if(dist > attackRange * TILE_WIDTH)
                {
                    targetEntityId = 0;
                }
            }
        }

        // Acquire nearest enemy within range if no target
        if(targetEntityId == 0 && delegates.findTarget)
        {
            targetEntityId = delegates.findTarget(*this);
        }

        if(targetEntityId != 0)
        {
            Entity *target = delegates.getEntity ? delegates.getEntity(targetEntityId) : nullptr;
            if(target && target->alive && target->state != ActionState::Die)
            {
                float dstX = target->getUnitCenterPixelX();
                float dstY = target->getUnitCenterPixelY();
                float dx = dstX - srcX;
                float dy = dstY - srcY;

                turretFacing = Utility::dir8FromVector(dx, dy);

                if(cooldownTimer <= 0.0f)
                {
                    cooldownTimer = nextCooldown();
                    attackAnimTimer = 0.15f;
                    state = ActionState::Attack;

                    float mSrcX, mSrcY;
                    getMuzzlePoint(mSrcX, mSrcY);
                    if(delegates.spawnProjectile)
                    {
                        delegates.spawnProjectile(*this, mSrcX, mSrcY, dstX, dstY, target->id, map);
                    }
                }
            }
        }
    }

} // namespace OpenArtOfWar
