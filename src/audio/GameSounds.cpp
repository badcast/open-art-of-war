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

        const char *cueEvent(EntityManager::SoundCue c)
        {
            using C = EntityManager::SoundCue;
            switch(c)
            {
                case C::FireRifle: return "fire_rifle";
                case C::FireMg: return "fire_mg";
                case C::FireSniper: return "fire_sniper";
                case C::FireCannon: return "fire_cannon";
                case C::FireRocket: return "fire_rocket";
                case C::FireMlrs: return "fire_mlrs";
                case C::FireFlame: return "fire_flame";
                case C::HitBullet: return "hit_bullet";
                case C::HitArmor: return "hit_armor";
                case C::ExplodeSmall: return "explode_small";
                case C::WallDestroyed: return "wall_destroyed";
            }
            return "";
        }

        bool infantry(const Entity &e)
        {
            const Unit *u = e.asUnit();
            return u && u->isInfantry();
        }

        // Heard by the local player: its own team's things always, the others only where its fog shows the ground
        bool hearable(const TileMap &map, bool fogEnabled, float x, float y, PlayerId owner)
        {
            if(!fogEnabled || (owner < kMaxPlayers && !Players::hostile(owner, Players::local())))
                return true;
            return map.getFogState(static_cast<int>(x / TILE_WIDTH), static_cast<int>(y / TILE_HEIGHT)) != FogState::Hidden;
        }

    } // namespace

    void GameSounds::reset()
    {
        m_seen.clear();
        m_primed = false;
        m_lowPower = false;
        m_radarStart = 0;
        m_selection.clear();
    }

    void GameSounds::update(SoundEngine &sound, EntityManager &entities, const Economy &economy, const TileMap &map, const Camera &camera, bool fogEnabled,
                            const RadarAlert &radar, uint64_t nowMs)
    {
        SfxMixer &mixer = sound.sfx();
        // The view in world pixels
        const float zoom = std::max(0.01f, camera.getZoom());
        const float viewX = camera.getX(), viewY = camera.getY();
        const float viewW = camera.getViewportWidth() / zoom, viewH = camera.getViewportHeight() / zoom;
        auto at = [&](const char *event, float x, float y)
        {
            const SfxMixer::EventConfig *cfg = mixer.config(event);
            if(!cfg)
                return;
            if(!cfg->positional)
            {
                sound.playSfx(event);
                return;
            }
            const SfxPlacement p = placeSfx(x, y, viewX, viewY, viewW, viewH, cfg->rangeTiles * TILE_WIDTH);
            if(p.audible)
                sound.playSfx(event, p.gain, p.pan);
        };

        // Shots, hits, shells landing, walls coming down
        for(const EntityManager::SoundCueEvent &c : entities.takeSoundCues())
        {
            if(hearable(map, fogEnabled, c.x, c.y, c.owner))
                at(cueEvent(c.cue), c.x, c.y);
        }

        // Deaths, finished buildings, new units, damage to the base: by comparing with the last look
        const PlayerId me = Players::local();
        bool baseHit = false;
        const auto &all = entities.getAllEntities();
        if(entities.generation() != m_generation) // the world was rebuilt (a new mission / match)
        {
            m_seen.clear();
            m_primed = false;
            m_generation = entities.generation();
        }
        for(const Entity &e : all)
        {
            auto it = m_seen.find(e.id);
            const bool dead = !e.alive || e.state == ActionState::Die;
            const Building *b = e.asBuilding();
            const bool complete = !b || b->buildProgress >= 1.0f;
            if(it == m_seen.end())
            {
                m_seen[e.id] = {dead, complete, e.currentHp, e.owner};
                // A fresh unit of the player next to its own barracks / factory: trained
                if(m_primed && !dead && !b && e.owner == me)
                {
                    for(const Entity &o : all)
                    {
                        const Building *pb = o.asBuilding();
                        if(pb && pb->owner == me && pb->isUnitProducer() && std::abs(pb->tileX - e.tileX) <= pb->buildWidth + 3 && std::abs(pb->tileY - e.tileY) <= pb->buildHeight + 3)
                        {
                            sound.announce("ann_unit_ready");
                            break;
                        }
                    }
                }
                continue;
            }
            Seen &s = it->second;
            if(m_primed && dead && !s.dead)
            {
                const float x = e.getUnitCenterPixelX(), y = e.getUnitCenterPixelY();
                if(hearable(map, fogEnabled, x, y, e.owner))
                    at(b ? "explode_large" : (infantry(e) ? "death_infantry" : "explode_large"), x, y);
                if(e.owner == me && !b)
                    sound.announce("ann_unit_lost");
                else if(e.owner < kMaxPlayers && Players::hostile(e.owner, me) && !infantry(e) && hearable(map, fogEnabled, x, y, e.owner))
                    sound.playVoice("target_destroyed");
            }
            if(m_primed && !dead && b && e.owner == me && complete && !s.complete)
                sound.announce("ann_construction_complete");
            if(m_primed && !dead && b && e.owner == me && e.currentHp < s.hp && complete)
                baseHit = true;
            s.dead = dead;
            s.complete = complete;
            s.hp = e.currentHp;
        }
        if(baseHit)
        {
            sound.playSfx("alert_base");
            sound.announce("ann_base_attack");
        }

        // Power running short
        const bool low = economy.hasPowerDeficit();
        if(m_primed && low && !m_lowPower)
            sound.announce("ann_low_power");
        m_lowPower = low;

        // An enemy on the radar
        if(radar.active() && radar.startMs() != m_radarStart)
        {
            if(m_primed)
            {
                sound.playSfx("alert_radar");
                sound.announce("ann_enemy_detected");
            }
            m_radarStart = radar.startMs();
        }
        (void) nowMs;
        m_primed = true;
    }

    void GameSounds::playReply(SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected, const char *what)
    {
        for(int id : selected)
        {
            const Entity *e = entities.getEntity(id);
            if(!e || !e->alive || e->isBuilding || e->owner != Players::local())
                continue;
            const std::string w = what;
            if(w == "hold")
            {
                sound.playVoice("ack_hold");
                return;
            }
            const std::string kind = infantry(*e) ? "infantry" : "vehicle";
            sound.playVoice("ack_" + w + "_" + kind);
            return;
        }
    }

    void GameSounds::selectionChanged(SoundEngine &sound, const EntityManager &entities, const std::vector<int> &selected)
    {
        bool someoneNew = false;
        for(int id : selected)
            someoneNew = someoneNew || std::find(m_selection.begin(), m_selection.end(), id) == m_selection.end();
        m_selection = selected;
        if(someoneNew)
            playReply(sound, entities, selected, "select");
    }

} // namespace OpenArtOfWar
