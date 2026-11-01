#include "game/Entity.hpp"
#include "core/Utility.hpp"
#include <cmath>

namespace OpenArtOfWar
{

    Entity::Entity(bool isBuildingVal) : isBuilding(isBuildingVal)
    {
    }

    float Entity::NextCooldown() const
    {
        return attackCooldownMax > attackCooldown ? Utility::RandomFloat(attackCooldown, attackCooldownMax) : attackCooldown;
    }

    void Entity::GetMuzzlePoint(float &outX, float &outY) const
    {
        float cx = GetUnitCenterPixelX();
        float cy = GetUnitCenterPixelY();
        int tf = (turretFacing % 8 + 8) % 8;
        const float *s_dirDx = Utility::kDir8X;
        const float *s_dirDy = Utility::kDir8Y;
        outX = cx + s_dirDx[tf] * 12.0f;
        outY = (cy - 4.0f) + s_dirDy[tf] * 8.0f;
    }

} // namespace OpenArtOfWar
