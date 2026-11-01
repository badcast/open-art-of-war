#pragma once

#include <cstdint>

namespace OpenArtOfWar
{

// The sounds the game itself triggers (GameSounds), named by constants: the code passes these, never a string.
// SoundIds.cpp holds the one table that maps them to the events of assets/data/audio_sfx.json / audio_voices.json,
// whose settings (volume, range, limits) are the only parameters of a sound.

// Effects heard where they happen, and the alerts
enum class WorldSfx : uint8_t
{
    FireRifle,
    FireMg,
    FireSniper,
    FireCannon,
    FireCannonHigh,
    FireRocket,
    FireMlrs,
    FireFlame,
    HitBullet,
    HitArmor,
    ExplodeSmall,
    ExplodeLarge,
    WallDestroyed,
    DeathInfantry,
    AlertBase,
    AlertRadar
};

// Lines of the announcer (queued, one at a time)
enum class Announcement : uint8_t
{
    ConstructionComplete,
    UnitReady,
    UnitLost,
    LowPower,
    BaseAttack,
    EnemyDetected
};

// What a unit says
enum class UnitReply : uint8_t
{
    Select,
    Move,
    Attack,
    Hold,
    TargetDestroyed
};

namespace SoundIds
{
// Event name in the sound settings
const char *Name(WorldSfx sound);
const char *Name(Announcement line);
// A reply has a line for infantry and one for vehicles (hold and target destroyed are the same for all)
const char *Name(UnitReply reply, bool infantry);
// "@select" / "@move" / "@attack" / "@hold" of SoundRequests; false for anything else
bool ReplyFromRequest(const char *request, UnitReply &out);
} // namespace SoundIds

} // namespace OpenArtOfWar
