#include "SpriteCatalog.hpp"

#include "core/Utility.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>

namespace CharEditor {

namespace {

namespace U = OpenArtOfWar::Utility;

constexpr int kMaxDepth = 12;
constexpr int kTileCount = 245;   // tiles per tileset, the rest of the list is decorations
constexpr int kPlainBuildings = 31; // buildings 0..30: structures, walls (all damage steps) and the scaffolding

// Kinds of a child reference in the graphics tables
constexpr int kSprite = 0;
constexpr int kComposite = 1;
constexpr int kAnim = 2;

const char* kindWord(int kind) { return kind == kComposite ? "composite" : kind == kAnim ? "anim" : "sprite"; }

nlohmann::json readJson(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return nlohmann::json::parse(in);
}

int intOf(const nlohmann::json& j, const char* key, int fallback = 0) {
    return j.contains(key) && j[key].is_number() ? j[key].get<int>() : fallback;
}

} // namespace

const char* spriteGroupName(SpriteGroup group) {
    switch (group) {
        case SpriteGroup::Units: return "Units";
        case SpriteGroup::Buildings: return "Buildings";
        case SpriteGroup::Terrain: return "Terrain";
        default: return "Other";
    }
}

const char* spriteGroupSlug(SpriteGroup group) {
    switch (group) {
        case SpriteGroup::Units: return "units";
        case SpriteGroup::Buildings: return "buildings";
        case SpriteGroup::Terrain: return "terrain";
        default: return "other";
    }
}

std::string graphicsDigest(nlohmann::json entry) {
    entry.erase("digest");
    const std::string text = entry.dump();
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    char out[24];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(hash));
    return out;
}

std::string spriteSlug(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "object" : out;
}

SpriteGroup SpriteCatalog::groupOfSprite(int spriteId) const {
    if (spriteId < 0 || spriteId >= static_cast<int>(spriteGroup.size())) return SpriteGroup::Other;
    return static_cast<SpriteGroup>(spriteGroup[static_cast<size_t>(spriteId)]);
}

std::set<int> SpriteCatalog::spritesOfSequence(int sequenceId) const {
    SpriteObject tmp;
    collectSequence(sequenceId, tmp);
    return tmp.sprites;
}

// ---------------------------------------------------------------------------------------------------------
// Dependency closure
// ---------------------------------------------------------------------------------------------------------

void SpriteCatalog::collectChild(int kind, int id, SpriteObject& out, int depth) const {
    if (kind == kComposite) collectComposite(id, out, depth);
    else if (kind == kAnim) collectAnim(id, out, depth);
    else if (id >= 0 && id < static_cast<int>(rawSprites.size())) out.sprites.insert(id);
}

void SpriteCatalog::collectComposite(int id, SpriteObject& out, int depth) const {
    if (depth > kMaxDepth || id < 0 || id >= compCount || !out.composites.insert(id).second) return;
    const auto& comp = gd["composite_sprites"];
    const auto range = comp["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        const auto& part = comp["parts"][static_cast<size_t>(i)];
        collectChild(part["is_comp"].get<int>() == 1 ? kComposite : kSprite, part["child"].get<int>(), out, depth + 1);
    }
}

void SpriteCatalog::collectAnim(int id, SpriteObject& out, int depth) const {
    if (depth > kMaxDepth || id < 0 || id >= animCount || !out.anims.insert(id).second) return;
    const auto& anim = gd["anim_sequences"];
    const auto range = anim["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        const auto& frame = anim["frames"][static_cast<size_t>(i)];
        collectChild(frame["is_comp"].get<int>() == 1 ? kComposite : kSprite, frame["child"].get<int>(), out, depth + 1);
    }
}

void SpriteCatalog::collectBody(int id, SpriteObject& out) const {
    if (id < 0 || id >= bodyCount || !out.bodies.insert(id).second) return;
    const auto& bodies = gd["unit_bodies"];
    const auto range = bodies["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        const auto& layer = bodies["layers"][static_cast<size_t>(i)];
        collectChild(layer["is_comp"].get<int>(), layer["child"].get<int>(), out, 1);
    }
}

void SpriteCatalog::collectSequence(int id, SpriteObject& out) const {
    if (id < 0 || id >= seqCount || !out.sequences.insert(id).second) return;
    const auto& seqs = gd["unit_sequences"];
    const auto range = seqs["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        collectBody(seqs["steps"][static_cast<size_t>(i)]["body"].get<int>(), out);
    }
}

void SpriteCatalog::collectBuilding(int id, SpriteObject& out) const {
    if (id < 0 || id >= buildingCount || !out.buildings.insert(id).second) return;
    const auto& blds = gd["buildings"];
    const auto range = blds["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        const auto& part = blds["parts"][static_cast<size_t>(i)];
        collectChild(part["is_comp"].get<int>(), part["child"].get<int>(), out, 1);
    }
}

void SpriteCatalog::finish(SpriteObject& object) const {
    for (int s : object.rootSequences) collectSequence(s, object);
    for (int c : object.rootComposites) collectComposite(c, object, 0);
    for (int a : object.rootAnims) collectAnim(a, object, 0);
    for (int b : object.rootBuildings) collectBuilding(b, object);
}

// ---------------------------------------------------------------------------------------------------------
// Loading and grouping
// ---------------------------------------------------------------------------------------------------------

bool SpriteCatalog::load(const fs::path& assetsRoot, std::string& error) {
    rawSprites.clear();
    objs.clear();
    spriteGroup.clear();
    try {
        const nlohmann::json sp = readJson(assetsRoot / "characters" / "sprites.json");
        gd = readJson(assetsRoot / "data" / "graphics_data.json");
        rawSprites.reserve(sp.size());
        for (const auto& s : sp) {
            RawSprite r;
            r.id = intOf(s, "id", static_cast<int>(rawSprites.size()));
            r.page = intOf(s, "page");
            r.x = intOf(s, "x");
            r.y = intOf(s, "y");
            r.w = intOf(s, "width");
            r.h = intOf(s, "height");
            r.palette = intOf(s, "palette");
            rawSprites.push_back(r);
        }
        // Ids are indices everywhere in the tables
        for (size_t i = 0; i < rawSprites.size(); ++i) rawSprites[i].id = static_cast<int>(i);
        compCount = static_cast<int>(gd["composite_sprites"]["ranges"].size());
        animCount = static_cast<int>(gd["anim_sequences"]["ranges"].size());
        bodyCount = static_cast<int>(gd["unit_bodies"]["ranges"].size());
        seqCount = static_cast<int>(gd["unit_sequences"]["ranges"].size());
        buildingCount = static_cast<int>(gd["buildings"]["ranges"].size());
    } catch (const std::exception& e) {
        error = std::string("Sprite data: ") + e.what();
        return false;
    }

    // Extra roots of the units from their character files: death sequences and the wreck
    struct UnitExtra { std::vector<int> sequences, composites; };
    std::map<int, UnitExtra> extra;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(assetsRoot / "characters", ec)) {
        if (entry.path().extension() != ".yaml" || entry.path().filename().string().rfind("research_", 0) == 0) continue;
        try {
            const YAML::Node doc = YAML::LoadFile(entry.path().string());
            if (!doc["archetype_id"] || doc["type"].as<std::string>("") != "unit") continue;
            UnitExtra& e = extra[doc["archetype_id"].as<int>()];
            const YAML::Node anim = doc["animations"];
            for (const char* key : {"death_sequences", "flame_death_sequences"}) {
                if (anim && anim[key]) for (const auto& v : anim[key]) e.sequences.push_back(v.as<int>());
            }
            if (anim && anim["wreck_composite"]) {
                const int first = anim["wreck_composite"].as<int>();
                const int stages = anim["wreck_stages"].as<int>(1);
                for (int k = 0; k < stages; ++k) e.composites.push_back(first + k);
            }
        } catch (const std::exception&) {}
    }

    // Units: idle and walk sequences have 8 facings, the infantry of the first four archetypes also a firing pose (+8);
    // the other base sequences (death, transformation, siege) are single
    const auto& bases = gd["unit_archetypes"];
    for (int a = 0; a < U::kArchetypeCount && a < static_cast<int>(bases.size()); ++a) {
        const U::ArchetypeInfo* info = U::archetypeInfo(a);
        if (!info) continue;
        SpriteObject o;
        o.kind = SpriteObjectKind::Unit;
        o.group = SpriteGroup::Units;
        o.kindName = "unit";
        o.faction = U::factionName(info->faction);
        o.id = a;
        o.name = info->name;
        o.slug = spriteSlug(o.name);
        const bool form = info->baseFormId >= 0; // siege / baseline forms have single sequences
        if (!bases[a].empty()) o.thumbSequence = bases[a][0].get<int>() + (form ? 0 : 2);
        for (size_t k = 0; k < bases[a].size(); ++k) {
            const int base = bases[a][k].get<int>();
            if (!form && k < 2) {
                for (int f = 0; f < 8; ++f) o.rootSequences.push_back(base + f);
                if (k == 0 && (15 & (1 << a))) for (int f = 8; f < 16; ++f) o.rootSequences.push_back(base + f);
            } else {
                o.rootSequences.push_back(base);
            }
        }
        const auto it = extra.find(a);
        if (it != extra.end()) {
            o.rootSequences.insert(o.rootSequences.end(), it->second.sequences.begin(), it->second.sequences.end());
            o.rootComposites = it->second.composites;
        }
        std::sort(o.rootSequences.begin(), o.rootSequences.end());
        o.rootSequences.erase(std::unique(o.rootSequences.begin(), o.rootSequences.end()), o.rootSequences.end());
        finish(o);
        objs.push_back(std::move(o));
    }

    // Structures, walls and the scaffolding
    for (int b = 0; b < kPlainBuildings && b < buildingCount; ++b) {
        SpriteObject o;
        o.kind = SpriteObjectKind::Building;
        o.group = SpriteGroup::Buildings;
        o.kindName = "building";
        o.id = b;
        if (const U::BuildingKindInfo* info = U::buildingKindInfo(b)) {
            o.name = info->name;
            o.faction = U::factionName(info->faction);
        } else if (b >= 15 && b <= 19) {
            o.name = "Concrete Wall damage " + std::to_string(b - 15);
            o.faction = "Confederation";
        } else if (b >= 20 && b <= 24) {
            o.name = "Reinforced Wall damage " + std::to_string(b - 20);
            o.faction = "Confederation";
        } else if (b >= 25 && b <= 29) {
            o.name = "Wall damage " + std::to_string(b - 25);
            o.faction = "Resistance";
        } else {
            o.name = b == 30 ? "Construction scaffolding" : "Building " + std::to_string(b);
            o.faction = "Neutral";
        }
        o.slug = spriteSlug(o.name);
        o.rootBuildings = {b};
        finish(o);
        objs.push_back(std::move(o));
    }

    // Terrain: the tiles of a tileset and its decorations (buildings of the map)
    const auto& tilesets = gd["tilesets"];
    for (int t = 0; t < static_cast<int>(tilesets.size()); ++t) {
        SpriteObject o;
        o.kind = SpriteObjectKind::Tileset;
        o.group = SpriteGroup::Terrain;
        o.kindName = "tileset";
        o.faction = "Neutral";
        o.id = t;
        o.name = "Tileset " + std::to_string(t);
        o.slug = spriteSlug(o.name);
        const auto& list = tilesets[static_cast<size_t>(t)];
        for (int i = 0; i < kTileCount && i < static_cast<int>(list.size()); ++i) o.rootComposites.push_back(list[static_cast<size_t>(i)].get<int>());
        const int offset = gd["decor_offsets"].size() > static_cast<size_t>(t) ? gd["decor_offsets"][static_cast<size_t>(t)].get<int>() : kTileCount;
        std::set<int> decor;
        for (int i = offset; i < static_cast<int>(list.size()); ++i) {
            const int b = list[static_cast<size_t>(i)].get<int>();
            if (b >= 0 && b < buildingCount) decor.insert(b);
        }
        o.rootBuildings.assign(decor.begin(), decor.end());
        finish(o);
        objs.push_back(std::move(o));
    }

    // Everything the objects above do not reach (interface, effects, projectiles, rubble...) belongs to the leftovers
    SpriteObject rest;
    rest.kind = SpriteObjectKind::Misc;
    rest.group = SpriteGroup::Other;
    rest.kindName = "misc";
    rest.faction = "Neutral";
    rest.id = 0;
    rest.name = "Unassigned graphics";
    rest.slug = spriteSlug(rest.name);
    std::set<int> seqs, comps, anims, blds;
    for (const auto& o : objs) {
        seqs.insert(o.sequences.begin(), o.sequences.end());
        comps.insert(o.composites.begin(), o.composites.end());
        anims.insert(o.anims.begin(), o.anims.end());
        blds.insert(o.buildings.begin(), o.buildings.end());
    }
    for (int i = 0; i < seqCount; ++i) if (!seqs.count(i)) rest.rootSequences.push_back(i);
    for (int i = 0; i < compCount; ++i) if (!comps.count(i)) rest.rootComposites.push_back(i);
    for (int i = 0; i < animCount; ++i) if (!anims.count(i)) rest.rootAnims.push_back(i);
    for (int i = 0; i < buildingCount; ++i) if (!blds.count(i)) rest.rootBuildings.push_back(i);
    finish(rest);

    // Sprites nothing uses at all, one object per sheet
    std::vector<bool> used(rawSprites.size(), false);
    for (const auto& o : objs) for (int s : o.sprites) used[static_cast<size_t>(s)] = true;
    for (int s : rest.sprites) used[static_cast<size_t>(s)] = true;
    objs.push_back(std::move(rest));
    std::map<int, SpriteObject> loose;
    for (const RawSprite& s : rawSprites) {
        if (used[static_cast<size_t>(s.id)]) continue;
        SpriteObject& o = loose[s.page];
        if (o.name.empty()) {
            o.kind = SpriteObjectKind::Misc;
            o.group = SpriteGroup::Other;
            o.kindName = "misc";
            o.faction = "Neutral";
            o.id = 1 + s.page;
            o.name = "Loose sprites sheet " + std::to_string(s.page);
            o.slug = spriteSlug(o.name);
        }
        o.sprites.insert(s.id);
    }
    for (auto& [page, o] : loose) objs.push_back(std::move(o));

    // Faction order (Resistance, Confederation, the rest) inside the groups, ids as they are
    auto factionRank = [](const std::string& f) { return f == "Resistance" ? 0 : f == "Confederation" ? 1 : 2; };
    std::stable_sort(objs.begin(), objs.end(), [&](const SpriteObject& a, const SpriteObject& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.kind == SpriteObjectKind::Unit && b.kind == SpriteObjectKind::Unit) {
            const int fa = factionRank(a.faction), fb = factionRank(b.faction);
            if (fa != fb) return fa < fb;
        }
        return false;
    });

    // Every sprite goes to the first group (units, buildings, terrain, other) that uses it
    spriteGroup.assign(rawSprites.size(), static_cast<uint8_t>(SpriteGroup::Other));
    std::vector<bool> assigned(rawSprites.size(), false);
    for (int g = 0; g < kSpriteGroupCount; ++g) {
        for (const auto& o : objs) {
            if (static_cast<int>(o.group) != g) continue;
            for (int s : o.sprites) {
                if (assigned[static_cast<size_t>(s)]) continue;
                assigned[static_cast<size_t>(s)] = true;
                spriteGroup[static_cast<size_t>(s)] = static_cast<uint8_t>(g);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Dependency tree (for the editor)
// ---------------------------------------------------------------------------------------------------------

DepNode SpriteCatalog::childNode(int kind, int id, int ox, int oy, int depth) const {
    DepNode n;
    if (kind == kComposite) n = compositeNode(id, depth + 1);
    else if (kind == kAnim) n = animNode(id);
    else {
        n.kind = "sprite";
        n.id = id;
        n.label = "Sprite #" + std::to_string(id);
    }
    n.ox = ox;
    n.oy = oy;
    return n;
}

DepNode SpriteCatalog::compositeNode(int id, int depth) const {
    DepNode n;
    n.kind = "composite";
    n.id = id;
    n.label = "Composite #" + std::to_string(id);
    if (depth > 6 || id < 0 || id >= compCount) return n;
    const auto& comp = gd["composite_sprites"];
    const auto range = comp["ranges"][static_cast<size_t>(id)];
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
        const auto& part = comp["parts"][static_cast<size_t>(i)];
        n.children.push_back(childNode(part["is_comp"].get<int>() == 1 ? kComposite : kSprite, part["child"].get<int>(),
                                       part["ox"].get<int>(), part["oy"].get<int>(), depth));
    }
    return n;
}

DepNode SpriteCatalog::animNode(int id) const {
    DepNode n;
    n.kind = "anim";
    n.id = id;
    n.label = "Animation #" + std::to_string(id);
    if (id < 0 || id >= animCount) return n;
    const auto& anim = gd["anim_sequences"];
    const auto range = anim["ranges"][static_cast<size_t>(id)];
    int k = 0;
    for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i, ++k) {
        const auto& frame = anim["frames"][static_cast<size_t>(i)];
        DepNode f = childNode(frame["is_comp"].get<int>() == 1 ? kComposite : kSprite, frame["child"].get<int>(),
                              frame["ox"].get<int>(), frame["oy"].get<int>(), 3);
        f.label = "Frame " + std::to_string(k) + ": " + f.label;
        n.children.push_back(std::move(f));
    }
    return n;
}

DepNode SpriteCatalog::tree(const SpriteObject& o) const {
    DepNode root;
    root.kind = "object";
    root.id = o.id;
    root.label = o.name;

    const auto& seqs = gd["unit_sequences"];
    const auto& bodies = gd["unit_bodies"];
    for (int s : o.rootSequences) {
        if (s < 0 || s >= seqCount) continue;
        DepNode sn;
        sn.kind = "sequence";
        sn.id = s;
        sn.label = "Sequence #" + std::to_string(s);
        const auto range = seqs["ranges"][static_cast<size_t>(s)];
        int k = 0;
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i, ++k) {
            const auto& step = seqs["steps"][static_cast<size_t>(i)];
            DepNode st;
            st.kind = "step";
            st.id = i;
            st.ox = step["ox"].get<int>();
            st.oy = step["oy"].get<int>();
            const int body = step["body"].get<int>();
            st.label = "Step " + std::to_string(k) + " (body #" + std::to_string(body) + ")";
            if (body >= 0 && body < bodyCount) {
                const auto br = bodies["ranges"][static_cast<size_t>(body)];
                for (int l = br[0].get<int>(); l <= br[1].get<int>(); ++l) {
                    const auto& layer = bodies["layers"][static_cast<size_t>(l)];
                    DepNode ln = childNode(layer["is_comp"].get<int>(), layer["child"].get<int>(), layer["ox"].get<int>(),
                                           layer["oy"].get<int>(), 1);
                    ln.label = "Layer (flag " + std::to_string(layer["flag"].get<int>()) + "): " + ln.label;
                    st.children.push_back(std::move(ln));
                }
            }
            sn.children.push_back(std::move(st));
        }
        root.children.push_back(std::move(sn));
    }
    for (int c : o.rootComposites) root.children.push_back(compositeNode(c, 0));
    for (int a : o.rootAnims) root.children.push_back(animNode(a));
    const auto& blds = gd["buildings"];
    for (int b : o.rootBuildings) {
        if (b < 0 || b >= buildingCount) continue;
        DepNode bn;
        bn.kind = "building";
        bn.id = b;
        bn.label = "Building #" + std::to_string(b);
        const auto range = blds["ranges"][static_cast<size_t>(b)];
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
            const auto& part = blds["parts"][static_cast<size_t>(i)];
            DepNode pn = childNode(part["is_comp"].get<int>(), part["child"].get<int>(), part["ox"].get<int>(),
                                   part["oy"].get<int>(), 1);
            pn.label = "Part (layer " + std::to_string(part["layer"].get<int>()) + ", state " +
                       std::to_string(part["state"].get<int>()) + "): " + pn.label;
            bn.children.push_back(std::move(pn));
        }
        root.children.push_back(std::move(bn));
    }
    return root;
}

// ---------------------------------------------------------------------------------------------------------
// JSON of an object
// ---------------------------------------------------------------------------------------------------------

nlohmann::json SpriteCatalog::describe(const SpriteObject& o) const {
    using nlohmann::json;
    json out = json::object();
    out["roots"] = {{"sequences", o.rootSequences}, {"composites", o.rootComposites},
                    {"animations", o.rootAnims}, {"buildings", o.rootBuildings}};

    auto childJson = [](const json& src, int kind) {
        return json{{"kind", kindWord(kind)}, {"child", src["child"]}, {"ox", src["ox"]}, {"oy", src["oy"]}};
    };

    json seqs = json::array();
    for (int s : o.sequences) {
        const auto range = gd["unit_sequences"]["ranges"][static_cast<size_t>(s)];
        json steps = json::array();
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) steps.push_back(gd["unit_sequences"]["steps"][static_cast<size_t>(i)]);
        { json e = {{"id", s}, {"steps", steps}}; e["digest"] = graphicsDigest(e); seqs.push_back(e); }
    }
    out["sequences"] = seqs;

    json bodies = json::array();
    for (int b : o.bodies) {
        const auto range = gd["unit_bodies"]["ranges"][static_cast<size_t>(b)];
        json layers = json::array();
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
            const auto& l = gd["unit_bodies"]["layers"][static_cast<size_t>(i)];
            json item = childJson(l, l["is_comp"].get<int>());
            item["flag"] = l["flag"];
            layers.push_back(item);
        }
        { json e = {{"id", b}, {"layers", layers}}; e["digest"] = graphicsDigest(e); bodies.push_back(e); }
    }
    out["bodies"] = bodies;

    json comps = json::array();
    for (int c : o.composites) {
        const auto range = gd["composite_sprites"]["ranges"][static_cast<size_t>(c)];
        json parts = json::array();
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
            const auto& p = gd["composite_sprites"]["parts"][static_cast<size_t>(i)];
            parts.push_back(childJson(p, p["is_comp"].get<int>() == 1 ? kComposite : kSprite));
        }
        { json e = {{"id", c}, {"parts", parts}}; e["digest"] = graphicsDigest(e); comps.push_back(e); }
    }
    out["composites"] = comps;

    json anims = json::array();
    for (int a : o.anims) {
        const auto range = gd["anim_sequences"]["ranges"][static_cast<size_t>(a)];
        json frames = json::array();
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
            const auto& f = gd["anim_sequences"]["frames"][static_cast<size_t>(i)];
            frames.push_back(childJson(f, f["is_comp"].get<int>() == 1 ? kComposite : kSprite));
        }
        { json e = {{"id", a}, {"frames", frames}}; e["digest"] = graphicsDigest(e); anims.push_back(e); }
    }
    out["animations"] = anims;

    json blds = json::array();
    for (int b : o.buildings) {
        const auto range = gd["buildings"]["ranges"][static_cast<size_t>(b)];
        json parts = json::array();
        for (int i = range[0].get<int>(); i <= range[1].get<int>(); ++i) {
            const auto& p = gd["buildings"]["parts"][static_cast<size_t>(i)];
            json item = childJson(p, p["is_comp"].get<int>());
            item["layer"] = p["layer"];
            item["state"] = p["state"];
            parts.push_back(item);
        }
        json entry = {{"id", b},
                      {"width", gd["buildings"]["widths"][static_cast<size_t>(b)]},
                      {"height", gd["buildings"]["heights"][static_cast<size_t>(b)]},
                      {"parts", parts}};
        entry["digest"] = graphicsDigest(entry);
        blds.push_back(entry);
    }
    out["buildings"] = blds;
    return out;
}

} // namespace CharEditor
