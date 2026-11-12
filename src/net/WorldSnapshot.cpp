#include "net/WorldSnapshot.hpp"
#include "game/Players.hpp"

#include "game/Building.hpp"
#include "game/EntityManager.hpp"
#include "game/TileMap.hpp"
#include "game/Unit.hpp"
#include "openaow/net/BitStream.hpp"

#include <algorithm>
#include <cmath>

namespace OpenArtOfWar
{

namespace
{
constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxEntities = 20000;
constexpr float kSubStep = 16.0f; // sub-tile offsets in 1/16 pixel

std::int32_t Quantise(float v)
{
    return static_cast<std::int32_t>(std::lround(v * kSubStep));
}
float Dequantise(std::int64_t q)
{
    return static_cast<float>(q) / kSubStep;
}
bool SameSub(float a, float b)
{
    return Quantise(a) == Quantise(b);
}
} // namespace

bool EntitySnap::operator==(const EntitySnap &o) const
{
    return id == o.id && isBuilding == o.isBuilding && type == o.type && archetype == o.archetype
           && rawBuilding == o.rawBuilding && faction == o.faction && owner == o.owner && tileX == o.tileX
           && tileY == o.tileY && SameSub(subX, o.subX) && SameSub(subY, o.subY) && hp == o.hp && maxHp == o.maxHp
           && state == o.state && alive == o.alive && facing == o.facing && turret == o.turret && target == o.target
           && orderX == o.orderX && orderY == o.orderY && siege == o.siege && buildPermille == o.buildPermille
           && powered == o.powered && producing == o.producing && repairing == o.repairing && rallyX == o.rallyX
           && rallyY == o.rallyY && queue == o.queue;
}

bool WorldSnapshot::operator==(const WorldSnapshot &o) const
{
    return tick == o.tick && baseTick == o.baseTick && full == o.full && entities == o.entities && removed == o.removed
           && hiddenAlive == o.hiddenAlive && economies == o.economies && research == o.research && mines == o.mines && result == o.result && hasWalls == o.hasWalls && walls == o.walls && projectiles == o.projectiles && cues == o.cues && blasts == o.blasts
           && explosions == o.explosions && deaths == o.deaths && groundMarks == o.groundMarks;
}

WorldSnapshot WorldSnapshot::DeltaFrom(const WorldSnapshot &base) const
{
    WorldSnapshot delta = *this;
    delta.full = false;
    delta.baseTick = base.tick;
    delta.entities.clear();
    delta.removed.clear();
    auto old = base.entities.begin();
    auto current = entities.begin();
    while (old != base.entities.end() || current != entities.end())
    {
        if (current == entities.end() || (old != base.entities.end() && old->id < current->id))
            delta.removed.push_back((old++)->id);
        else if (old == base.entities.end() || current->id < old->id)
            delta.entities.push_back(*current++);
        else
        {
            if (!(*current == *old))
                delta.entities.push_back(*current);
            ++current;
            ++old;
        }
    }
    if (economies == base.economies)
        delta.economies.clear();
    if (research == base.research && tick % 20 != 0)
        delta.research.clear();
    if (hasWalls && base.hasWalls && walls == base.walls)
    {
        delta.hasWalls = false;
        delta.walls.clear();
    }
    return delta;
}

bool WorldSnapshot::ExpandFrom(const WorldSnapshot &base)
{
    if (full || base.tick != baseTick || !base.full)
        return false;
    auto merged = base.entities;
    for (auto id : removed)
        merged.erase(std::remove_if(merged.begin(), merged.end(), [id](const EntitySnap &e) { return e.id == id; }), merged.end());
    for (const auto &e : entities)
    {
        auto it = std::lower_bound(merged.begin(), merged.end(), e.id, [](const EntitySnap &a, std::uint32_t id) { return a.id < id; });
        if (it != merged.end() && it->id == e.id)
            *it = e;
        else
            merged.insert(it, e);
    }
    entities = std::move(merged);
    if (economies.empty())
        economies = base.economies;
    if (research.empty())
        research = base.research;
    removed.clear();
    full = true;
    return true;
}

void WorldSnapshot::FilterForPlayer(const EntityManager &manager, const TileMap &map, std::uint8_t player)
{
    // Victory / defeat of this recipient: out of the match, or every hostile player is
    if (player < kMaxPlayers && Players::IsActive(player))
    {
        bool enemyLeft = false;
        for (int p = 0; p < kMaxPlayers; ++p)
            if (p != player && Players::IsActive(static_cast<PlayerId>(p)) && Players::Hostile(static_cast<PlayerId>(p), player)
                && !manager.IsEliminated(static_cast<PlayerId>(p)))
                enemyLeft = true;
        result = manager.IsEliminated(player) ? 2 : (enemyLeft ? 0 : 1);
    }
    economies.erase(std::remove_if(economies.begin(), economies.end(), [player](const EconomySnap &e) {
        return Players::Hostile(e.player, player);
    }), economies.end());
    research.erase(std::remove_if(research.begin(), research.end(), [player](const ResearchSnap &r) {
        return Players::Hostile(r.player, player);
    }), research.end());
    // The other side never sees a mine (EntityManager::RenderMines), fog or not
    mines.erase(std::remove_if(mines.begin(), mines.end(), [&](const MineSnap &m) {
        return player < kMaxPlayers && Players::Hostile(m.owner, player);
    }), mines.end());
    if (!map.IsFogEnabled())
        return;
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const EntitySnap &s) {
        const Entity *e = manager.GetEntity(static_cast<int>(s.id));
        if (e && !manager.IsEntityVisibleToPlayer(*e, player, map))
        {
            hiddenAlive.push_back(s.id);
            return true;
        }
        return false;
    }), entities.end());
}

WorldSnapshot WorldSnapshot::CaptureEntities(const EntityManager &entities, std::uint32_t tick)
{
    WorldSnapshot s;
    s.tick = tick;
    s.full = true;
    for (const auto &m : entities.GetMines())
        s.mines.push_back({m.id, m.tileX, m.tileY, m.kind, m.owner, m.armLeft});
    for (const Entity &e : entities.GetAllEntities())
    {
        // The dead and dying are not sent: a replica does not run their death (wrecks, rubble come with the effects)
        if (!e.alive || e.state == ActionState::Die)
            continue;
        EntitySnap n;
        n.id = static_cast<std::uint32_t>(e.id);
        n.isBuilding = e.AsBuilding() != nullptr;
        n.type = static_cast<std::int32_t>(e.type);
        n.faction = static_cast<std::uint8_t>(e.faction);
        n.owner = static_cast<std::uint8_t>(e.owner);
        n.tileX = e.tileX;
        n.tileY = e.tileY;
        n.subX = e.subX;
        n.subY = e.subY;
        n.hp = e.currentHp;
        n.maxHp = e.maxHp;
        n.state = static_cast<std::uint8_t>(e.state);
        n.alive = e.alive;
        n.turret = e.turretFacing;
        n.target = static_cast<std::uint32_t>(std::max(0, e.targetEntityId));
        if (const Unit *u = e.AsUnit())
        {
            n.archetype = u->archetypeId;
            n.facing = u->facingDirection;
            n.orderX = u->orderX;
            n.orderY = u->orderY;
            n.siege = u->isSiegeMode;
        }
        if (const Building *b = e.AsBuilding())
        {
            n.rawBuilding = b->rawBuildingType;
            n.buildPermille = static_cast<std::uint16_t>(std::clamp(b->buildProgress, 0.0f, 1.0f) * 1000.0f);
            n.powered = b->isPowered;
            n.producing = b->isProducing;
            n.repairing = b->repairing;
            n.rallyX = b->rallyTileX;
            n.rallyY = b->rallyTileY;
            for (const ProductionItem &item : b->productionQueue)
            {
                const float done = item.totalTime > 0.0f ? item.progressTime / item.totalTime : 1.0f;
                n.queue.push_back({item.archetype, static_cast<std::uint16_t>(std::clamp(done, 0.0f, 1.0f) * 1000.0f)});
            }
        }
        s.entities.push_back(std::move(n));
    }
    std::sort(s.entities.begin(), s.entities.end(), [](const EntitySnap &a, const EntitySnap &b) { return a.id < b.id; });
    return s;
}

void WorldSnapshot::CaptureWalls(const TileMap &map)
{
    hasWalls = true;
    walls.clear();
    for (int y = 0; y < map.GetHeight(); ++y)
    {
        for (int x = 0; x < map.GetWidth(); ++x)
        {
            if (map.GetWall(x, y) == 0)
                continue;
            const int hp = map.GetWallHp(x, y);
            const int max = std::max(1, map.GetWallMaxHp(x, y));
            walls.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                static_cast<std::uint8_t>(map.GetWallFaction(x, y)),
                static_cast<std::uint8_t>(std::clamp(hp * 100 / max, 1, 100)),
                static_cast<std::uint8_t>(map.GetWallOwner(x, y))});
        }
    }
}

void WorldSnapshot::ApplyWalls(TileMap &map) const
{
    if (!hasWalls)
        return;
    for (int y = 0; y < map.GetHeight(); ++y)
        for (int x = 0; x < map.GetWidth(); ++x)
            if (map.GetWall(x, y) != 0)
                map.SetWall(x, y, Faction::Neutral, 0); // gone unless the list has it again
    for (const WallSnap &w : walls)
        map.SetWall(w.x, w.y, static_cast<Faction>(w.faction), w.hpPercent, w.owner);
}

void WorldSnapshot::CaptureEffects(EntityManager &entities)
{
    projectiles.clear();
    for (const Projectile &p : entities.GetProjectiles())
    {
        if (!p.active)
            continue;
        projectiles.push_back({static_cast<std::uint8_t>(p.type), p.x, p.y, static_cast<std::uint8_t>(p.direction),
            static_cast<std::uint8_t>(p.dir32), static_cast<std::uint8_t>(p.faction), static_cast<std::uint8_t>(p.owner)});
    }
    deaths.clear();
    for (const Entity &e : entities.GetAllEntities())
    {
        if (e.state != ActionState::Die || !entities.CollectedDeaths().insert(e.id).second)
            continue;
        DeathSnap d;
        d.id = static_cast<std::uint32_t>(e.id);
        d.tileX = e.tileX; d.tileY = e.tileY; d.subX = e.subX; d.subY = e.subY; d.timer = e.deathTimer;
        if (const Unit *u = e.AsUnit())
        {
            d.flame = u->killedByFlame; d.sequence = u->selectedDeathSeq;
            d.wreckComposite = u->wreckComposite; d.wreckStages = u->wreckStages;
        }
        if (const Building *b = e.AsBuilding())
        {
            d.wreckComposite = b->wreckComposite; d.wreckStages = b->wreckStages;
        }
        deaths.push_back(d);
    }
    EntityManager::ReplicaEvents events = entities.TakeReplicaEvents();
    groundMarks.clear();
    for (const auto &m : events.groundMarks)
        groundMarks.push_back({m.x, m.y, m.composite, m.seconds});
    cues.clear();
    for (const EntityManager::SoundCueEvent &c : events.cues)
        cues.push_back({static_cast<std::uint8_t>(c.cue), c.x, c.y, static_cast<std::uint8_t>(c.owner),
            entities.SoundName(c.sound), c.gain});
    blasts.clear();
    for (const EntityManager::BuildingBlast &b : events.blasts)
        blasts.push_back({b.x, b.y, b.size});
    explosions.clear();
    for (const EntityManager::ReplicaExplosion &e : events.explosions)
        explosions.push_back({e.x, e.y, e.anim, e.delay});
}

std::vector<std::uint8_t> WorldSnapshot::Encode() const
{
    std::vector<std::uint8_t> buffer(kMaxBytes);
    Net::BitWriter w(buffer.data(), buffer.size());
    w.writeU8(kVersion);
    w.writeU32(tick);
    w.writeU32(baseTick);
    w.writeBool(full);
    w.writeVarUint(entities.size());
    for (const EntitySnap &e : entities)
    {
        w.writeVarUint(e.id);
        w.writeBool(e.isBuilding);
        w.writeVarInt(e.type);
        w.writeVarInt(e.archetype);
        w.writeVarInt(e.rawBuilding);
        w.writeU8(e.faction);
        w.writeU8(e.owner);
        w.writeVarInt(e.tileX);
        w.writeVarInt(e.tileY);
        w.writeVarInt(Quantise(e.subX));
        w.writeVarInt(Quantise(e.subY));
        w.writeVarInt(e.hp);
        w.writeVarInt(e.maxHp);
        w.writeU8(e.state);
        w.writeBool(e.alive);
        w.writeVarInt(e.facing);
        w.writeVarInt(e.turret);
        w.writeVarUint(e.target);
        w.writeVarInt(e.orderX);
        w.writeVarInt(e.orderY);
        w.writeBool(e.siege);
        if (e.isBuilding)
        {
            w.writeU16(e.buildPermille);
            w.writeBool(e.powered);
            w.writeBool(e.producing);
            w.writeBool(e.repairing);
            w.writeVarInt(e.rallyX);
            w.writeVarInt(e.rallyY);
            w.writeVarUint(e.queue.size());
            for (const ProductionSnap &p : e.queue)
            {
                w.writeVarInt(p.archetype);
                w.writeU16(p.progressPermille);
            }
        }
    }
    w.writeIdList(removed);
    w.writeIdList(hiddenAlive);
    w.writeVarUint(economies.size());
    for (const EconomySnap &s : economies)
    {
        w.writeU8(s.player);
        w.writeVarInt(s.credits);
        w.writeVarInt(s.powerOut);
        w.writeVarInt(s.powerUse);
    }
    w.writeVarUint(research.size());
    for (const ResearchSnap &s : research)
    {
        w.writeU8(s.player);
        w.writeU32(s.completed);
        w.writeVarInt(s.active);
        w.writeFloat(s.timer);
        w.writeFloat(s.duration);
    }
    w.writeVarUint(mines.size());
    for (const MineSnap &m : mines)
    {
        w.writeVarInt(m.id);
        w.writeVarInt(m.tileX);
        w.writeVarInt(m.tileY);
        w.writeVarInt(m.kind);
        w.writeU8(m.owner);
        w.writeFloat(m.armLeft);
    }
    w.writeU8(result);
    w.writeBool(hasWalls);
    if (hasWalls)
    {
        w.writeVarUint(walls.size());
        for (const WallSnap &t : walls)
        {
            w.writeVarInt(t.x);
            w.writeVarInt(t.y);
            w.writeU8(t.faction);
            w.writeU8(t.hpPercent);
            w.writeU8(t.owner);
        }
    }
    w.writeVarUint(projectiles.size());
    for (const ProjectileSnap &p : projectiles)
    {
        w.writeU8(p.type);
        w.writeFloat(p.x);
        w.writeFloat(p.y);
        w.writeU8(p.direction);
        w.writeU8(p.dir32);
        w.writeU8(p.faction);
        w.writeU8(p.owner);
    }
    w.writeVarUint(cues.size());
    for (const CueSnap &c : cues)
    {
        w.writeU8(c.cue);
        w.writeFloat(c.x);
        w.writeFloat(c.y);
        w.writeU8(c.owner);
        w.writeString(c.sound, 64);
        w.writeFloat(c.gain);
    }
    w.writeVarUint(blasts.size());
    for (const BlastSnap &b : blasts)
    {
        w.writeFloat(b.x);
        w.writeFloat(b.y);
        w.writeFloat(b.size);
    }
    w.writeVarUint(explosions.size());
    for (const ExplosionSnap &e : explosions)
    {
        w.writeFloat(e.x);
        w.writeFloat(e.y);
        w.writeVarInt(e.anim);
        w.writeFloat(e.delay);
    }
    w.writeVarUint(deaths.size());
    for (const DeathSnap &d : deaths)
    {
        w.writeVarUint(d.id); w.writeVarInt(d.tileX); w.writeVarInt(d.tileY);
        w.writeFloat(d.subX); w.writeFloat(d.subY); w.writeFloat(d.timer);
        w.writeVarInt(d.sequence); w.writeVarInt(d.wreckComposite); w.writeVarInt(d.wreckStages);
        w.writeBool(d.flame);
    }
    w.writeVarUint(groundMarks.size());
    for (const GroundMarkSnap &m : groundMarks)
    {
        w.writeFloat(m.x); w.writeFloat(m.y); w.writeVarInt(m.composite); w.writeFloat(m.seconds);
    }
    const std::size_t size = w.finish();
    if (w.overflowed())
        return {};
    buffer.resize(size);
    return buffer;
}

bool WorldSnapshot::Decode(const std::uint8_t *data, std::size_t size, WorldSnapshot &out)
{
    Net::BitReader r(data, size);
    if (r.readU8() != kVersion)
        return false;
    WorldSnapshot s;
    s.tick = r.readU32();
    s.baseTick = r.readU32();
    s.full = r.readBool();
    const std::uint64_t count = r.readVarUint();
    if (r.failed() || count > kMaxEntities)
        return false;
    s.entities.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count && !r.failed(); ++i)
    {
        EntitySnap e;
        e.id = static_cast<std::uint32_t>(r.readVarUint());
        e.isBuilding = r.readBool();
        e.type = static_cast<std::int32_t>(r.readVarInt());
        e.archetype = static_cast<std::int32_t>(r.readVarInt());
        e.rawBuilding = static_cast<std::int32_t>(r.readVarInt());
        e.faction = r.readU8();
        e.owner = r.readU8();
        e.tileX = static_cast<std::int32_t>(r.readVarInt());
        e.tileY = static_cast<std::int32_t>(r.readVarInt());
        e.subX = Dequantise(r.readVarInt());
        e.subY = Dequantise(r.readVarInt());
        e.hp = static_cast<std::int32_t>(r.readVarInt());
        e.maxHp = static_cast<std::int32_t>(r.readVarInt());
        e.state = r.readU8();
        e.alive = r.readBool();
        e.facing = static_cast<std::int32_t>(r.readVarInt());
        e.turret = static_cast<std::int32_t>(r.readVarInt());
        e.target = static_cast<std::uint32_t>(r.readVarUint());
        e.orderX = static_cast<std::int32_t>(r.readVarInt());
        e.orderY = static_cast<std::int32_t>(r.readVarInt());
        e.siege = r.readBool();
        if (e.isBuilding)
        {
            e.buildPermille = r.readU16();
            e.powered = r.readBool();
            e.producing = r.readBool();
            e.repairing = r.readBool();
            e.rallyX = static_cast<std::int32_t>(r.readVarInt());
            e.rallyY = static_cast<std::int32_t>(r.readVarInt());
            const std::uint64_t items = r.readVarUint();
            if (items > 64)
                return false;
            for (std::uint64_t k = 0; k < items && !r.failed(); ++k)
            {
                ProductionSnap p;
                p.archetype = static_cast<std::int32_t>(r.readVarInt());
                p.progressPermille = r.readU16();
                e.queue.push_back(p);
            }
        }
        s.entities.push_back(std::move(e));
    }
    if (!r.readIdList(s.removed, kMaxEntities) || !r.readIdList(s.hiddenAlive, kMaxEntities))
        return false;
    const std::uint64_t econ = r.readVarUint();
    if (r.failed() || econ > 64)
        return false;
    for (std::uint64_t i = 0; i < econ && !r.failed(); ++i)
    {
        EconomySnap e;
        e.player = r.readU8();
        e.credits = static_cast<std::int32_t>(r.readVarInt());
        e.powerOut = static_cast<std::int32_t>(r.readVarInt());
        e.powerUse = static_cast<std::int32_t>(r.readVarInt());
        s.economies.push_back(e);
    }
    const std::uint64_t researchCount = r.readVarUint();
    if (r.failed() || researchCount > 64)
        return false;
    for (std::uint64_t i = 0; i < researchCount && !r.failed(); ++i)
    {
        ResearchSnap item;
        item.player = r.readU8();
        item.completed = r.readU32();
        item.active = static_cast<std::int32_t>(r.readVarInt());
        item.timer = r.readFloat();
        item.duration = r.readFloat();
        s.research.push_back(item);
    }
    const std::uint64_t mineCount = r.readVarUint();
    if (r.failed() || mineCount > kMaxEntities)
        return false;
    for (std::uint64_t i = 0; i < mineCount && !r.failed(); ++i)
    {
        MineSnap m;
        m.id = static_cast<std::int32_t>(r.readVarInt());
        m.tileX = static_cast<std::int32_t>(r.readVarInt());
        m.tileY = static_cast<std::int32_t>(r.readVarInt());
        m.kind = static_cast<std::int32_t>(r.readVarInt());
        m.owner = r.readU8();
        m.armLeft = r.readFloat();
        s.mines.push_back(m);
    }
    s.result = r.readU8();
    if (s.result > 2)
        return false;
    s.hasWalls = r.readBool();
    if (s.hasWalls)
    {
        const std::uint64_t wallCount = r.readVarUint();
        if (r.failed() || wallCount > 70000)
            return false;
        for (std::uint64_t i = 0; i < wallCount && !r.failed(); ++i)
        {
            WallSnap t;
            t.x = static_cast<std::int16_t>(r.readVarInt());
            t.y = static_cast<std::int16_t>(r.readVarInt());
            t.faction = r.readU8();
            t.hpPercent = r.readU8();
            t.owner = r.readU8();
            s.walls.push_back(t);
        }
    }
    const std::uint64_t shots = r.readVarUint();
    if (r.failed() || shots > 4096)
        return false;
    for (std::uint64_t i = 0; i < shots && !r.failed(); ++i)
    {
        ProjectileSnap p;
        p.type = r.readU8();
        p.x = r.readFloat();
        p.y = r.readFloat();
        p.direction = r.readU8();
        p.dir32 = r.readU8();
        p.faction = r.readU8();
        p.owner = r.readU8();
        s.projectiles.push_back(p);
    }
    const std::uint64_t cueCount = r.readVarUint();
    if (r.failed() || cueCount > 1024)
        return false;
    for (std::uint64_t i = 0; i < cueCount && !r.failed(); ++i)
    {
        CueSnap c;
        c.cue = r.readU8();
        c.x = r.readFloat();
        c.y = r.readFloat();
        c.owner = r.readU8();
        c.sound = r.readString(64);
        c.gain = r.readFloat();
        s.cues.push_back(std::move(c));
    }
    const std::uint64_t blastCount = r.readVarUint();
    if (r.failed() || blastCount > 256)
        return false;
    for (std::uint64_t i = 0; i < blastCount && !r.failed(); ++i)
    {
        BlastSnap b;
        b.x = r.readFloat();
        b.y = r.readFloat();
        b.size = r.readFloat();
        s.blasts.push_back(b);
    }
    const std::uint64_t exCount = r.readVarUint();
    if (r.failed() || exCount > 1024)
        return false;
    for (std::uint64_t i = 0; i < exCount && !r.failed(); ++i)
    {
        ExplosionSnap e;
        e.x = r.readFloat();
        e.y = r.readFloat();
        e.anim = static_cast<std::int32_t>(r.readVarInt());
        e.delay = r.readFloat();
        s.explosions.push_back(e);
    }
    const std::uint64_t deathCount = r.readVarUint();
    if (r.failed() || deathCount > kMaxEntities)
        return false;
    for (std::uint64_t i = 0; i < deathCount && !r.failed(); ++i)
    {
        DeathSnap d;
        d.id = static_cast<std::uint32_t>(r.readVarUint());
        d.tileX = static_cast<std::int32_t>(r.readVarInt()); d.tileY = static_cast<std::int32_t>(r.readVarInt());
        d.subX = r.readFloat(); d.subY = r.readFloat(); d.timer = r.readFloat();
        d.sequence = static_cast<std::int32_t>(r.readVarInt());
        d.wreckComposite = static_cast<std::int32_t>(r.readVarInt());
        d.wreckStages = static_cast<std::int32_t>(r.readVarInt()); d.flame = r.readBool();
        s.deaths.push_back(d);
    }
    const std::uint64_t markCount = r.readVarUint();
    if (r.failed() || markCount > kMaxEntities)
        return false;
    for (std::uint64_t i = 0; i < markCount && !r.failed(); ++i)
    {
        GroundMarkSnap m;
        m.x = r.readFloat(); m.y = r.readFloat(); m.composite = static_cast<std::int32_t>(r.readVarInt());
        m.seconds = r.readFloat(); s.groundMarks.push_back(m);
    }
    if (r.failed() || !r.atEnd())
        return false;
    out = std::move(s);
    return true;
}

} // namespace OpenArtOfWar
