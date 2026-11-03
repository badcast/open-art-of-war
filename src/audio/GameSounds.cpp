#include "audio/GameSounds.hpp"
#include "audio/SfxMixer.hpp"
#include "audio/SoundEngine.hpp"
#include "core/Config.hpp"
#include "game/Building.hpp"
#include "game/Economy.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "graphics/Camera.hpp"
#include "ui/RadarAlert.hpp"

#include <algorithm>

namespace OpenArtOfWar
{

namespace
{

WorldSfx CueSound(EntityManager::SoundCue c)
{
    using C = EntityManager::SoundCue;
    switch (c)
    {
        case C::FireRifle:
            return WorldSfx::FireRifle;
        case C::FireMg:
            return WorldSfx::FireMg;
        case C::FireSniper:
            return WorldSfx::FireSniper;
        case C::FireCannon:
            return WorldSfx::FireCannon;
        case C::FireCannonHigh:
            return WorldSfx::FireCannonHigh;
        case C::FireRocket:
            return WorldSfx::FireRocket;
        case C::FireMlrs:
            return WorldSfx::FireMlrs;
        case C::FireFlame:
            return WorldSfx::FireFlame;
        case C::HitBullet:
            return WorldSfx::HitBullet;
        case C::HitArmor:
            return WorldSfx::HitArmor;
        case C::ExplodeSmall:
            return WorldSfx::ExplodeSmall;
        case C::WallDestroyed:
            return WorldSfx::WallDestroyed;
    }
    return WorldSfx::HitArmor;
}

bool Infantry(const Entity &e)
{
    const Unit *u = e.AsUnit();
    return u && u->IsInfantry();
}

// Heard by the local player: its own team's things always, the others only where its fog shows the ground
bool Hearable(const TileMap &map, bool fogEnabled, float x, float y, PlayerId owner)
{
    if (!fogEnabled || (owner < kMaxPlayers && !Players::Hostile(owner, Players::Local())))
        return true;
    return map.GetFogState(static_cast<int>(x / TILE_WIDTH), static_cast<int>(y / TILE_HEIGHT)) != FogState::Hidden;
}

} // namespace

void GameSounds::Reset()
{
    m_seen.clear();
    m_primed = false;
    m_lowPower = false;
    m_radarStart = 0;
    m_selection.clear();
}

void GameSounds::Update(
    SoundEngine &sound,
    EntityManager &entities,
    const Economy &economy,
    const TileMap &map,
    const Camera &camera,
    bool fogEnabled,
    const RadarAlert &radar,
    uint64_t nowMs)
{
    // The view in world pixels
    const float zoom = std::max(0.01f, camera.GetZoom());
    const float viewX = camera.GetX(), viewY = camera.GetY();
    const float viewW = camera.GetViewportWidth() / zoom, viewH = camera.GetViewportHeight() / zoom;
    // Range and the rest come from the sound's settings (audio_sfx.json), not from here
    // `own`: the event the character file names (empty, or without clips: the game's own `what`); `gain`: the
    // volume of that character / gun
    auto at = [&](WorldSfx what, float x, float y, const std::string &own = std::string(), float gain = 1.0f) {
        const SfxMixer::EventConfig *cfg = sound.SfxConfig(own);
        const std::string event = cfg ? own : std::string(SoundIds::Name(what));
        if (!cfg)
            cfg = sound.SfxConfig(what);
        if (!cfg || gain <= 0.0f)
            return;
        if (!cfg->positional)
        {
            sound.PlaySfx(event, std::min(1.0f, gain));
            return;
        }
        const SfxPlacement p = PlaceSfx(x, y, viewX, viewY, viewW, viewH, cfg->rangeTiles * TILE_WIDTH, zoom, cfg->minGain);
        if (p.audible)
            sound.PlaySfx(event, p.gain * gain, p.pan, p.muffle);
    };

    // Shots, hits, shells landing, walls coming down
    for (const EntityManager::SoundCueEvent &c : entities.TakeSoundCues())
    {
        if (Hearable(map, fogEnabled, c.x, c.y, c.owner))
            at(CueSound(c.cue), c.x, c.y, entities.SoundName(c.sound), c.gain);
    }

    // Deaths, finished buildings, new units, damage to the base: by comparing with the last look
    const PlayerId me = Players::Local();
    // A person who only watches (a custom match of computer players) has no base: the announcer tells nobody about
    // the computer player whose view is shown
    const PlayerId mine = me < kMaxPlayers && Players::Slot(me).human ? me : kNoPlayer;
    bool baseHit = false;
    const auto &all = entities.GetAllEntities();
    if (entities.Generation() != m_generation) // the world was rebuilt (a new mission / match)
    {
        m_seen.clear();
        m_primed = false;
        m_generation = entities.Generation();
    }
    for (const Entity &e : all)
    {
        auto it = m_seen.find(e.id);
        const bool dead = !e.alive || e.state == ActionState::Die;
        const Building *b = e.AsBuilding();
        const bool complete = !b || b->buildProgress >= 1.0f;
        if (it == m_seen.end())
        {
            m_seen[e.id] = {dead, complete, e.currentHp, e.owner};
            // A fresh unit of the player next to its own barracks / factory: trained
            if (m_primed && !dead && !b && e.owner == mine)
            {
                for (const Entity &o : all)
                {
                    const Building *pb = o.AsBuilding();
                    if (pb && pb->owner == mine && pb->IsUnitProducer()
                        && std::abs(pb->tileX - e.tileX) <= pb->buildWidth + 3
                        && std::abs(pb->tileY - e.tileY) <= pb->buildHeight + 3)
                    {
                        sound.Announce(Announcement::UnitReady);
                        break;
                    }
                }
            }
            continue;
        }
        Seen &s = it->second;
        if (m_primed && dead && !s.dead)
        {
            const float x = e.GetUnitCenterPixelX(), y = e.GetUnitCenterPixelY();
            if (Hearable(map, fogEnabled, x, y, e.owner))
            {
                const CharacterSounds *own = entities.SoundsOf(e); // its own death sound and volume, if it has them
                at(b ? WorldSfx::ExplodeLarge : (Infantry(e) ? WorldSfx::DeathInfantry : WorldSfx::ExplodeLarge), x, y,
                   own ? entities.SoundName(own->deathId) : std::string(), own ? own->volume : 1.0f);
            }
            if (e.owner == mine && !b)
                sound.Announce(Announcement::UnitLost);
            else if (
                e.owner < kMaxPlayers && Players::Hostile(e.owner, me) && !Infantry(e)
                && Hearable(map, fogEnabled, x, y, e.owner))
                sound.PlayVoice(UnitReply::TargetDestroyed, false);
        }
        if (m_primed && !dead && b && e.owner == mine && complete && !s.complete)
            sound.Announce(Announcement::ConstructionComplete);
        // A finished building of the player lost health. Not at the moment it is finished: the last tick of the
        // construction clamps the health that was gained by rounding down to the maximum (that is no attack)
        if (m_primed && !dead && b && e.owner == mine && e.currentHp < s.hp && complete && s.complete)
            baseHit = true;
        s.dead = dead;
        s.complete = complete;
        s.hp = e.currentHp;
    }
    if (baseHit)
    {
        sound.PlaySfx(WorldSfx::AlertBase);
        sound.Announce(Announcement::BaseAttack);
    }

    // Power running short
    const bool low = mine != kNoPlayer && economy.HasPowerDeficit();
    if (m_primed && low && !m_lowPower)
        sound.Announce(Announcement::LowPower);
    m_lowPower = low;

    // An enemy on the radar
    if (radar.Active() && radar.StartMs() != m_radarStart)
    {
        if (m_primed)
        {
            sound.PlaySfx(WorldSfx::AlertRadar);
            sound.Announce(Announcement::EnemyDetected);
        }
        m_radarStart = radar.StartMs();
    }
    (void) nowMs;
    m_primed = true;
}

void GameSounds::PlayReply(
    SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected, UnitReply reply)
{
    for (int id : selected)
    {
        const Entity *e = entities.GetEntity(id);
        if (!e || !e->alive || e->isBuilding || e->owner != Players::Local())
            continue;
        sound.PlayVoice(reply, Infantry(*e));
        return;
    }
}

void GameSounds::SelectionChanged(SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected)
{
    bool someoneNew = false;
    for (int id : selected)
        someoneNew = someoneNew || std::find(m_selection.begin(), m_selection.end(), id) == m_selection.end();
    m_selection = selected;
    if (someoneNew)
        PlayReply(sound, entities, selected, UnitReply::Select);
}

} // namespace OpenArtOfWar
