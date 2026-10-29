#include "game/Entity.hpp"
#include "core/Utility.hpp"
#include <cmath>

namespace OpenArtOfWar
{

    Entity::Entity(bool isBuildingVal) : isBuilding(isBuildingVal)
    {
    }

    float Entity::nextCooldown() const
    {
        return attackCooldownMax > attackCooldown ? Utility::randomFloat(attackCooldown, attackCooldownMax) : attackCooldown;
    }

    void Entity::getMuzzlePoint(float &outX, float &outY) const
    {
        float cx = getUnitCenterPixelX();
        float cy = getUnitCenterPixelY();
        int tf = (turretFacing % 8 + 8) % 8;
        const float *s_dirDx = Utility::kDir8X;
        const float *s_dirDy = Utility::kDir8Y;
        outX = cx + s_dirDx[tf] * 12.0f;
        outY = (cy - 4.0f) + s_dirDy[tf] * 8.0f;
    }

} // namespace OpenArtOfWar
