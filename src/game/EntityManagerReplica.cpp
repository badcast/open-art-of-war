// A replica's world: state sent by the authority is written into the entities (TASKNEW.md step 3).
// Nothing here runs behaviour; the entity classes only hold the values the renderer and the HUD read.
#include "game/Building.hpp"
#include "game/EntityManager.hpp"
#include "game/Projectiles.hpp"
#include "game/Unit.hpp"
#include "net/WorldSnapshot.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace OpenArtOfWar
{

bool EntityManager::ApplySnapshot(const WorldSnapshot &snapshot)
{
    m_replicaWorld = true;
    for (const DeathSnap &d : snapshot.deaths)
    {
        Entity *e = GetEntity(static_cast<int>(d.id));
        if (!e || e->state == ActionState::Die || m_replicaVisible.count(e->id) == 0)
            continue;
        e->state = ActionState::Die; e->currentHp = 0; e->selected = false;
        e->tileX = d.tileX; e->tileY = d.tileY; e->subX = d.subX; e->subY = d.subY;
        e->deathTimer = d.timer; e->animTimer = 0.0f; e->animFrame = 0;
        if (Unit *u = e->AsUnit())
        {
            u->killedByFlame = d.flame; u->selectedDeathSeq = d.sequence;
            u->wreckComposite = d.wreckComposite; u->wreckStages = d.wreckStages;
        }
        if (Building *b = e->AsBuilding())
        {
            b->wreckComposite = d.wreckComposite; b->wreckStages = d.wreckStages;
        }
    }
    m_replicaVisible.clear();
    for (const EntitySnap &e : snapshot.entities)
        if (e.alive && static_cast<ActionState>(e.state) != ActionState::Die)
            m_replicaVisible.insert(static_cast<int>(e.id));
    m_mines.clear();
    for (const MineSnap &m : snapshot.mines)
        m_mines.push_back({m.id, m.owner, m.tileX, m.tileY, m.kind, m.armLeft});
    SnapshotRenderState(); // the picture glides from where things were to where the snapshot puts them

    // Entities that are over on the authority (dead or dying) and ones of another kind under the same id go first
    std::unordered_set<int> listed;
    std::unordered_set<int> drop;
    for (const EntitySnap &s : snapshot.entities)
    {
        if (s.alive && static_cast<ActionState>(s.state) != ActionState::Die)
            listed.insert(static_cast<int>(s.id));
        else
            drop.insert(static_cast<int>(s.id));
    }
    for (const std::uint32_t id : snapshot.hiddenAlive)
        listed.insert(static_cast<int>(id));
    for (const std::uint32_t id : snapshot.removed)
        drop.insert(static_cast<int>(id));
    m_entities.Raw().erase(
        std::remove_if(
            m_entities.Raw().begin(), m_entities.Raw().end(),
            [&](const std::unique_ptr<Entity> &e) {
                if (e->state == ActionState::Die)
                    return false;
                if (drop.count(e->id) != 0)
                    return true;
                if (snapshot.full && listed.count(e->id) == 0)
                    return true;
                for (const EntitySnap &s : snapshot.entities) // another kind under the same id
                    if (s.id == static_cast<std::uint32_t>(e->id) && s.alive)
                    {
                        if (s.isBuilding != (e->AsBuilding() != nullptr))
                            return true;
                        const Unit *u = e->AsUnit();
                        return u && u->archetypeId != s.archetype;
                    }
                return false;
            }),
        m_entities.Raw().end());

    std::unordered_map<int, Entity *> known;
    known.reserve(m_entities.size() * 2);
    for (Entity &e : m_entities)
        known[e.id] = &e;

    m_replicaApplying = true; // the authority decides how many there are: no slot limit here
    bool ok = true;
    for (const EntitySnap &s : snapshot.entities)
    {
        const int id = static_cast<int>(s.id);
        if (listed.count(id) == 0)
            continue;
        Entity *e = nullptr;
        if (const auto it = known.find(id); it != known.end())
            e = it->second;
        else
        {
            // Spawn under the authority's id; the id counter never goes back
            const int next = m_nextId;
            m_nextId = id;
            const Faction faction = static_cast<Faction>(s.faction);
            int made = 0;
            if (s.isBuilding)
                made = SpawnBuildingByIndex(s.rawBuilding, s.tileX, s.tileY, faction, 1.0f, s.owner);
            else
                made = SpawnUnitByArchetype(s.archetype, s.tileX, s.tileY, faction, s.owner);
            m_nextId = std::max(next, id + 1);
            if (made != id)
            {
                ok = false;
                continue;
            }
            e = &m_entities[m_entities.size() - 1];
            known[id] = e;
        }

        e->owner = s.owner;
        e->tileX = s.tileX;
        e->tileY = s.tileY;
        e->subX = s.subX;
        e->subY = s.subY;
        e->currentHp = s.hp;
        e->maxHp = s.maxHp;
        e->state = static_cast<ActionState>(s.state);
        e->alive = s.alive;
        e->turretFacing = s.turret;
        e->targetEntityId = static_cast<int>(s.target);
        if (Unit *u = e->AsUnit())
        {
            u->facingDirection = s.facing;
            u->orderX = s.orderX;
            u->orderY = s.orderY;
            u->isSiegeMode = s.siege;
        }
        if (Building *b = e->AsBuilding())
        {
            b->buildProgress = static_cast<float>(s.buildPermille) / 1000.0f;
            b->isPowered = s.powered;
            b->isProducing = s.producing;
            b->repairing = s.repairing;
            b->rallyTileX = s.rallyX;
            b->rallyTileY = s.rallyY;
            b->productionQueue.clear();
            for (const ProductionSnap &p : s.queue)
            {
                ProductionItem item;
                item.archetype = p.archetype;
                item.totalTime = 1.0f;
                item.progressTime = static_cast<float>(p.progressPermille) / 1000.0f;
                b->productionQueue.push_back(item);
            }
        }
    }

    // Shots in flight, as the authority has them now
    m_projectiles.clear();
    for (const ProjectileSnap &p : snapshot.projectiles)
    {
        Projectile shot;
        shot.x = shot.startX = shot.targetX = p.x;
        shot.y = shot.startY = shot.targetY = p.y;
        shot.type = static_cast<ProjectileType>(std::min<int>(p.type, Projectiles::kCount - 1));
        shot.direction = p.direction;
        shot.dir32 = p.dir32;
        shot.faction = static_cast<Faction>(p.faction);
        shot.owner = p.owner;
        shot.active = true;
        m_projectiles.push_back(shot);
    }
    for (const GroundMarkSnap &m : snapshot.groundMarks)
        AddGroundMark(m.x, m.y, m.composite, m.seconds);
    // Sounds, blasts and explosions of the ticks since the last snapshot: the same consumers as on the authority
    for (const CueSnap &c : snapshot.cues)
    {
        if (m_soundCues.size() < 256)
            m_soundCues.push_back({static_cast<SoundCue>(c.cue), c.x, c.y, c.owner, c.sound.empty() ? std::uint16_t(0) : InternSound(c.sound), c.gain});
    }
    for (const BlastSnap &b : snapshot.blasts)
    {
        if (m_buildingBlasts.size() < 64)
            m_buildingBlasts.push_back({b.x, b.y, b.size});
    }
    for (const ExplosionSnap &e : snapshot.explosions)
        SpawnExplosion(e.x, e.y, e.anim, e.delay);

    m_replicaApplying = false;
    return ok;
}

} // namespace OpenArtOfWar
