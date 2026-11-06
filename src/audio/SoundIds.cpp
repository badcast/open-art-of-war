#include "audio/SoundIds.hpp"

#include <cstring>

namespace OpenArtOfWar::SoundIds
{

const char *Name(WorldSfx sound)
{
    switch (sound)
    {
        case WorldSfx::FireRifle:
            return "fire_rifle";
        case WorldSfx::FireMg:
            return "fire_mg";
        case WorldSfx::FireSniper:
            return "fire_sniper";
        case WorldSfx::FireCannon:
            return "fire_cannon";
        case WorldSfx::FireCannonHigh:
            return "fire_cannon_high";
        case WorldSfx::FireRocket:
            return "fire_rocket";
        case WorldSfx::FireMlrs:
            return "fire_mlrs";
        case WorldSfx::FireFlame:
            return "fire_flame";
        case WorldSfx::HitBullet:
            return "hit_bullet";
        case WorldSfx::HitArmor:
            return "hit_armor";
        case WorldSfx::ExplodeSmall:
            return "explode_small";
        case WorldSfx::ExplodeLarge:
            return "explode_large";
        case WorldSfx::WallDestroyed:
            return "wall_destroyed";
        case WorldSfx::DeathInfantry:
            return "death_infantry";
        case WorldSfx::AlertBase:
            return "alert_base";
        case WorldSfx::AlertRadar:
            return "alert_radar";
    }
    return "";
}

const char *Name(Announcement line)
{
    switch (line)
    {
        case Announcement::ConstructionComplete:
            return "ann_construction_complete";
        case Announcement::UnitReady:
            return "ann_unit_ready";
        case Announcement::UnitLost:
            return "ann_unit_lost";
        case Announcement::LowPower:
            return "ann_low_power";
        case Announcement::BaseAttack:
            return "ann_base_attack";
        case Announcement::EnemyDetected:
            return "ann_enemy_detected";
    }
    return "";
}

const char *Name(UnitReply reply, bool infantry)
{
    switch (reply)
    {
        case UnitReply::Select:
            return infantry ? "ack_select_infantry" : "ack_select_vehicle";
        case UnitReply::Move:
            return infantry ? "ack_move_infantry" : "ack_move_vehicle";
        case UnitReply::Attack:
            return infantry ? "ack_attack_infantry" : "ack_attack_vehicle";
        case UnitReply::Hold:
            return "ack_hold";
        case UnitReply::TargetDestroyed:
            return "target_destroyed";
    }
    return "";
}

bool ReplyFromRequest(const char *request, UnitReply &out)
{
    if (!request)
        return false;
    struct Entry
    {
        const char *text;
        UnitReply reply;
    };
    static const Entry kEntries[]
        = {{"@select", UnitReply::Select},
           {"@move", UnitReply::Move},
           {"@attack", UnitReply::Attack},
           {"@hold", UnitReply::Hold}};
    for (const Entry &e : kEntries)
    {
        if (std::strcmp(request, e.text) == 0)
        {
            out = e.reply;
            return true;
        }
    }
    return false;
}

} // namespace OpenArtOfWar::SoundIds
