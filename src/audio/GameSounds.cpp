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

const char *CueEvent(EntityManager::SoundCue c)
{
    using C = EntityManager::SoundCue;
    switch (c)
    {
        case C::FireRifle:
            return "fire_rifle";
        case C::FireMg:
            return "fire_mg";
        case C::FireSniper:
            return "fire_sniper";
        case C::FireCannon:
            return "fire_cannon";
        case C::FireRocket:
            return "fire_rocket";
        case C::FireMlrs:
            return "fire_mlrs";
        case C::FireFlame:
            return "fire_flame";
        case C::HitBullet:
            return "hit_bullet";
        case C::HitArmor:
            return "hit_armor";
        case C::ExplodeSmall:
            return "explode_small";
        case C::WallDestroyed:
            return "wall_destroyed";
    }
    return "";
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
    SfxMixer &mixer = sound.Sfx();
    // The view in world pixels
    const float zoom = std::max(0.01f, camera.GetZoom());
    const float viewX = camera.GetX(), viewY = camera.GetY();
    const float viewW = camera.GetViewportWidth() / zoom, viewH = camera.GetViewportHeight() / zoom;
    auto at = [&](const char *event, float x, float y) {
        const SfxMixer::EventConfig *cfg = mixer.Config(event);
        if (!cfg)
            return;
        if (!cfg->positional)
        {
            sound.PlaySfx(event);
            return;
        }
        const SfxPlacement p = PlaceSfx(x, y, viewX, viewY, viewW, viewH, cfg->rangeTiles * TILE_WIDTH);
        if (p.audible)
            sound.PlaySfx(event, p.gain, p.pan);
    };

    // Shots, hits, shells landing, walls coming down
    for (const EntityManager::SoundCueEvent &c : entities.TakeSoundCues())
    {
        if (Hearable(map, fogEnabled, c.x, c.y, c.owner))
            at(CueEvent(c.cue), c.x, c.y);
    }

    // Deaths, finished buildings, new units, damage to the base: by comparing with the last look
    const PlayerId me = Players::Local();
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
            if (m_primed && !dead && !b && e.owner == me)
            {
                for (const Entity &o : all)
                {
                    const Building *pb = o.AsBuilding();
                    if (pb && pb->owner == me && pb->IsUnitProducer()
                        && std::abs(pb->tileX - e.tileX) <= pb->buildWidth + 3
                        && std::abs(pb->tileY - e.tileY) <= pb->buildHeight + 3)
                    {
                        sound.Announce("ann_unit_ready");
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
                at(b ? "explode_large" : (Infantry(e) ? "death_infantry" : "explode_large"), x, y);
            if (e.owner == me && !b)
                sound.Announce("ann_unit_lost");
            else if (
                e.owner < kMaxPlayers && Players::Hostile(e.owner, me) && !Infantry(e)
                && Hearable(map, fogEnabled, x, y, e.owner))
                sound.PlayVoice("target_destroyed");
        }
        if (m_primed && !dead && b && e.owner == me && complete && !s.complete)
            sound.Announce("ann_construction_complete");
        if (m_primed && !dead && b && e.owner == me && e.currentHp < s.hp && complete)
            baseHit = true;
        s.dead = dead;
        s.complete = complete;
        s.hp = e.currentHp;
    }
    if (baseHit)
    {
        sound.PlaySfx("alert_base");
        sound.Announce("ann_base_attack");
    }

    // Power running short
    const bool low = economy.HasPowerDeficit();
    if (m_primed && low && !m_lowPower)
        sound.Announce("ann_low_power");
    m_lowPower = low;

    // An enemy on the radar
    if (radar.Active() && radar.StartMs() != m_radarStart)
    {
        if (m_primed)
        {
            sound.PlaySfx("alert_radar");
            sound.Announce("ann_enemy_detected");
        }
        m_radarStart = radar.StartMs();
    }
    (void) nowMs;
    m_primed = true;
}

void GameSounds::PlayReply(
    SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected, const char *what)
{
    for (int id : selected)
    {
        const Entity *e = entities.GetEntity(id);
        if (!e || !e->alive || e->isBuilding || e->owner != Players::Local())
            continue;
        const std::string w = what;
        if (w == "hold")
        {
            sound.PlayVoice("ack_hold");
            return;
        }
        const std::string kind = Infantry(*e) ? "infantry" : "vehicle";
        sound.PlayVoice("ack_" + w + "_" + kind);
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
        PlayReply(sound, entities, selected, "select");
}

} // namespace OpenArtOfWar
