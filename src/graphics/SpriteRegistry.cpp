#include "graphics/SpriteRegistry.hpp"

#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

namespace OpenArtOfWar
{

namespace
{
constexpr const char *kMetaPrefix = "sprite_";
constexpr const char *kMetaSuffix = "_meta.yaml";

// Width and height from the IHDR chunk of a PNG
bool PngSize(std::string_view path, int &w, int &h)
{
    std::vector<std::uint8_t> bytes;
    if (!Assets::ReadBytes(path, bytes) || bytes.size() < 24 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G'
        || bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
        return false;
    auto be = [&](int at) {
        return static_cast<int>((bytes[at] << 24) | (bytes[at + 1] << 16) | (bytes[at + 2] << 8) | bytes[at + 3]);
    };
    w = be(16);
    h = be(20);
    return w > 0 && h > 0;
}

bool EndsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

SpriteKind SectionKind(std::string_view section)
{
    if (section == "frames")
        return SpriteKind::Frame;
    if (section == "parts")
        return SpriteKind::Part;
    if (section == "anims")
        return SpriteKind::Anim;
    if (section == "bodies")
        return SpriteKind::Body;
    if (section == "sequences")
        return SpriteKind::Sequence;
    if (section == "buildings")
        return SpriteKind::Building;
    return SpriteKind::None;
}

// The entry key of a kind: {frame: x}, {part: x}, ...
const char *EntryKey(SpriteKind kind)
{
    switch (kind)
    {
        case SpriteKind::Frame:
            return "frame";
        case SpriteKind::Part:
            return "part";
        case SpriteKind::Anim:
            return "anim";
        case SpriteKind::Body:
            return "body";
        case SpriteKind::Sequence:
            return "sequence";
        case SpriteKind::Building:
            return "building";
        default:
            return "";
    }
}

// The isComp code the renderer expects for an entry's target
std::uint8_t IsCompCode(SpriteKind kind)
{
    switch (kind)
    {
        case SpriteKind::Part:
            return 1;
        case SpriteKind::Anim:
            return 2;
        default:
            return 0;
    }
}

// Which kinds an entry of a container may name
bool EntryAllowed(SpriteKind container, SpriteKind target)
{
    switch (container)
    {
        case SpriteKind::Part:
            return target == SpriteKind::Frame || target == SpriteKind::Part;
        case SpriteKind::Anim:
            return target == SpriteKind::Frame || target == SpriteKind::Part;
        case SpriteKind::Body:
        case SpriteKind::Building:
            return target == SpriteKind::Frame || target == SpriteKind::Part || target == SpriteKind::Anim;
        case SpriteKind::Sequence:
            return target == SpriteKind::Body;
        default:
            return false;
    }
}

// "none" / "blue" / "red" -> 0 / 1 / 2; -1 unknown
int ParseRecolor(std::string_view v)
{
    if (v == "none")
        return 0;
    if (v == "blue")
        return 1;
    if (v == "red")
        return 2;
    return -1;
}

template <typename T>
T Narrow(int v, std::string_view where, std::vector<std::string> &errors)
{
    constexpr int lo = static_cast<int>(std::numeric_limits<T>::min());
    constexpr int hi = static_cast<int>(std::numeric_limits<T>::max());
    if (v < lo || v > hi)
    {
        errors.push_back(std::string(where) + ": value " + std::to_string(v) + " out of range");
        v = std::clamp(v, lo, hi);
    }
    return static_cast<T>(v);
}
} // namespace

const char *SpriteKindName(SpriteKind kind)
{
    switch (kind)
    {
        case SpriteKind::Frame:
            return "frame";
        case SpriteKind::Part:
            return "part";
        case SpriteKind::Anim:
            return "anim";
        case SpriteKind::Body:
            return "body";
        case SpriteKind::Sequence:
            return "sequence";
        case SpriteKind::Building:
            return "building";
        default:
            return "none";
    }
}

void SpriteRegistry::Clear()
{
    m_objects.clear();
    m_errors.clear();
    m_tables = {};
    m_names.clear();
}

void SpriteRegistry::Error(std::string_view msg)
{
    m_errors.emplace_back(msg);
}

bool SpriteRegistry::LoadDirectory(std::string_view dir)
{
    for (const std::string &path : Assets::List(dir, ".yaml"))
    {
        const std::string file = path.substr(path.find_last_of('/') + 1);
        if (file.rfind(kMetaPrefix, 0) != 0 || !EndsWith(file, kMetaSuffix))
            continue;
        const std::string name = file.substr(
            std::char_traits<char>::length(kMetaPrefix),
            file.size() - std::char_traits<char>::length(kMetaPrefix) - std::char_traits<char>::length(kMetaSuffix));
        std::string text;
        if (!Assets::ReadText(path, text))
        {
            Error(path + ": cannot read");
            continue;
        }
        try
        {
            AddObject(name, YAML::Load(text), dir);
        }
        catch (const std::exception &e)
        {
            Error(path + ": " + e.what());
        }
    }
    return Compile();
}

void SpriteRegistry::AddObject(std::string_view name, const YAML::Node &meta, std::string_view dir)
{
    const std::string where = "sprite '" + std::string(name) + "'";
    if (name.empty() || name.find('.') != std::string_view::npos)
    {
        Error(where + ": an object name must be non-empty and have no '.'");
        return;
    }
    if (m_objects.count(name))
    {
        Error(where + ": defined twice");
        return;
    }
    Object obj;
    obj.name.assign(name);
    obj.image.object = obj.name;
    const std::string image = meta["image"] ? meta["image"].as<std::string>() : kMetaPrefix + obj.name + ".png";
    obj.image.path = dir.empty() ? image : std::string(dir) + "/" + image;
    obj.image.scale = meta["scale"] ? meta["scale"].as<float>() : 1.0f;
    if (obj.image.scale <= 0.0f)
    {
        Error(where + ": scale must be positive");
        obj.image.scale = 1.0f;
    }
    if (meta["recolor"])
    {
        const std::string rc = meta["recolor"].as<std::string>();
        const int v = ParseRecolor(rc);
        if (v < 0)
            Error(where + ": recolor must be none, blue or red, not '" + rc + "'");
        else
            obj.image.recolor = static_cast<SpriteRecolor>(v);
    }
    obj.image.terrain = meta["terrain"] && meta["terrain"].as<bool>();
    if (meta["resistance_image"])
    {
        const std::string alt = meta["resistance_image"].as<std::string>();
        obj.image.resistancePath = dir.empty() ? alt : std::string(dir) + "/" + alt;
    }

    auto parseEntries = [&](SpriteKind container, const YAML::Node &list, const std::string &at, Item &item) {
        if (!list.IsSequence())
        {
            Error(at + ": expected a list of entries");
            return;
        }
        for (const YAML::Node &e : list)
        {
            Entry entry;
            int found = 0;
            for (SpriteKind k : {SpriteKind::Frame, SpriteKind::Part, SpriteKind::Anim, SpriteKind::Body})
            {
                if (e[EntryKey(k)])
                {
                    entry.kind = k;
                    entry.ref = e[EntryKey(k)].as<std::string>();
                    ++found;
                }
            }
            if (found != 1)
            {
                Error(at + ": an entry needs exactly one of frame/part/anim/body");
                continue;
            }
            if (!EntryAllowed(container, entry.kind))
            {
                Error(at + ": a " + SpriteKindName(container) + " cannot hold a " + SpriteKindName(entry.kind));
                continue;
            }
            if (const YAML::Node off = e["at"])
            {
                if (!off.IsSequence() || off.size() != 2)
                    Error(at + ": 'at' must be [x, y]");
                else
                {
                    entry.ox = off[0].as<int>();
                    entry.oy = off[1].as<int>();
                }
            }
            entry.flag = e["flag"] ? e["flag"].as<int>() : 0;
            entry.layer = e["layer"] ? e["layer"].as<int>() : 0;
            entry.state = e["state"] ? e["state"].as<int>() : 0;
            item.entries.push_back(std::move(entry));
        }
    };

    for (const char *section : {"frames", "parts", "anims", "bodies", "sequences", "buildings"})
    {
        const YAML::Node node = meta[section];
        if (!node)
            continue;
        if (!node.IsMap())
        {
            Error(where + ": '" + section + "' must be a map");
            continue;
        }
        const SpriteKind kind = SectionKind(section);
        for (const auto &kv : node)
        {
            Item item;
            item.kind = kind;
            item.name = kv.first.as<std::string>();
            const std::string at = where + " " + SpriteKindName(kind) + " '" + item.name + "'";
            if (item.name.find('.') != std::string::npos)
            {
                Error(at + ": an item name must have no '.'");
                continue;
            }
            const YAML::Node v = kv.second;
            if (kind == SpriteKind::Frame)
            {
                const YAML::Node rect = v.IsMap() ? v["rect"] : v;
                if (!rect || !rect.IsSequence() || rect.size() != 4)
                {
                    Error(at + ": expected [x, y, w, h]");
                    continue;
                }
                for (int i = 0; i < 4; ++i)
                    item.rect[i] = rect[i].as<float>();
                if (v.IsMap() && v["size"])
                {
                    const YAML::Node size = v["size"];
                    if (!size.IsSequence() || size.size() != 2)
                        Error(at + ": size must be [w, h]");
                    else
                    {
                        item.size[0] = size[0].as<float>();
                        item.size[1] = size[1].as<float>();
                    }
                }
                if (v.IsMap() && v["palette"])
                    item.palette = v["palette"].as<int>();
                if (v.IsMap() && v["recolor"])
                {
                    item.recolor = ParseRecolor(v["recolor"].as<std::string>());
                    if (item.recolor < 0)
                        Error(at + ": recolor must be none, blue or red");
                }
            }
            else if (kind == SpriteKind::Building)
            {
                if (const YAML::Node size = v["size"])
                {
                    item.width = size[0].as<int>();
                    item.height = size[1].as<int>();
                }
                if (v["style"])
                {
                    const std::string st = v["style"].as<std::string>();
                    if (st == "structure")
                        item.style = BuildingStyle::Structure;
                    else if (st == "wall")
                        item.style = BuildingStyle::Wall;
                    else if (st == "decor")
                        item.style = BuildingStyle::Decor;
                    else if (st == "decor_flat")
                        item.style = BuildingStyle::DecorFlat;
                    else
                        Error(at + ": style must be structure, wall, decor or decor_flat, not '" + st + "'");
                }
                item.tall = v["tall"] && v["tall"].as<bool>();
                parseEntries(kind, v["parts"], at, item);
            }
            else
            {
                parseEntries(kind, v, at, item);
            }
            auto &index = obj.byName[static_cast<int>(kind)];
            if (index.count(item.name))
            {
                Error(at + ": defined twice");
                continue;
            }
            index[item.name] = obj.items.size();
            obj.items.push_back(std::move(item));
        }
    }

    if (const YAML::Node fams = meta["families"])
    {
        for (const auto &kv : fams)
        {
            FamilyDef fam;
            fam.name = kv.first.as<std::string>();
            const std::string at = where + " family '" + fam.name + "'";
            const YAML::Node v = kv.second;
            if (!v.IsMap() || v.size() != 1)
            {
                Error(at + ": expected {<section>: [names]}");
                continue;
            }
            const std::string section = v.begin()->first.as<std::string>();
            fam.kind = SectionKind(section);
            if (fam.kind == SpriteKind::None)
            {
                Error(at + ": unknown section '" + section + "'");
                continue;
            }
            for (const YAML::Node &m : v.begin()->second)
                fam.members.push_back(m.as<std::string>());
            if (FindFamilyDef(obj, fam.name))
            {
                Error(at + ": defined twice");
                continue;
            }
            obj.families.push_back(std::move(fam));
        }
    }

    // "image_size": the size of the image the rectangles were written for. An image scaled since (an HD redraw, x4)
    // scales the rectangles with it; the drawn sizes stay
    if (const YAML::Node declared = meta["image_size"])
    {
        int w = 0, h = 0;
        if (!declared.IsSequence() || declared.size() != 2 || declared[0].as<float>(0) <= 0
            || declared[1].as<float>(0) <= 0)
            Error(where + ": image_size must be [w, h]");
        else if (PngSize(obj.image.path, w, h))
        {
            const float kx = w / declared[0].as<float>(), ky = h / declared[1].as<float>();
            if (std::fabs(kx - 1.0f) > 1e-6f || std::fabs(ky - 1.0f) > 1e-6f)
                for (Item &item : obj.items)
                {
                    if (item.kind != SpriteKind::Frame)
                        continue;
                    if (item.size[0] < 0)
                        item.size[0] = item.rect[2] / obj.image.scale;
                    if (item.size[1] < 0)
                        item.size[1] = item.rect[3] / obj.image.scale;
                    item.rect[0] *= kx;
                    item.rect[2] *= kx;
                    item.rect[1] *= ky;
                    item.rect[3] *= ky;
                }
        }
    }
    m_objects.emplace(name, std::move(obj));
}

const SpriteRegistry::FamilyDef *SpriteRegistry::FindFamilyDef(const Object &obj, std::string_view name) const
{
    for (const FamilyDef &f : obj.families)
        if (f.name == name)
            return &f;
    return nullptr;
}

const SpriteRegistry::Item *SpriteRegistry::LookupItem(
    std::string_view fromObject, std::string_view ref, SpriteKind kind) const
{
    const auto dot = ref.find('.');
    const std::string_view objName = dot == std::string_view::npos ? fromObject : ref.substr(0, dot);
    const std::string_view itemName = dot == std::string_view::npos ? ref : ref.substr(dot + 1);
    const auto it = m_objects.find(objName);
    if (it == m_objects.end())
        return nullptr;
    const auto &index = it->second.byName[static_cast<int>(kind)];
    const auto found = index.find(itemName);
    return found == index.end() ? nullptr : &it->second.items[found->second];
}

SpriteRef SpriteRegistry::LookupRef(
    std::string_view fromObject, std::string_view ref, SpriteKind kind, std::string_view where)
{
    if (const Item *item = LookupItem(fromObject, ref, kind))
        return {kind, item->handle};
    const auto dot = ref.find('.');
    const std::string_view objName = dot == std::string_view::npos ? fromObject : ref.substr(0, dot);
    const auto it = m_objects.find(objName);
    if (it == m_objects.end())
    {
        Error(
            std::string(where) + ": no sprite object '" + std::string(objName) + "' (for '" + std::string(ref) + "')");
        return {};
    }
    const SpriteRef r = FamilyMember(it->second, dot == std::string_view::npos ? ref : ref.substr(dot + 1), kind);
    if (r.Valid())
        return r;
    Error(std::string(where) + ": no " + std::string(SpriteKindName(kind)) + " '" + std::string(ref) + "'");
    return {};
}

SpriteRef SpriteRegistry::FamilyMember(const Object &obj, std::string_view name, SpriteKind kind) const
{
    // "family" or "family[i]"
    std::string_view famName = name;
    int index = 0;
    const auto open = name.find('[');
    if (open != std::string_view::npos)
    {
        if (name.back() != ']')
            return {};
        famName = name.substr(0, open);
        try
        {
            index = std::stoi(std::string(name.substr(open + 1, name.size() - open - 2)));
        }
        catch (const std::exception &)
        {
            return {};
        }
    }
    const FamilyDef *fam = FindFamilyDef(obj, famName);
    if (!fam || (kind != SpriteKind::None && fam->kind != kind))
        return {};
    return fam->compiled.At(index);
}

int SpriteRegistry::Allocate(SpriteKind kind)
{
    switch (kind)
    {
        case SpriteKind::Frame:
            m_tables.frames.emplace_back();
            return static_cast<int>(m_tables.frames.size()) - 1;
        case SpriteKind::Part:
            m_tables.compositeRanges.emplace_back();
            return static_cast<int>(m_tables.compositeRanges.size()) - 1;
        case SpriteKind::Anim:
            m_tables.animRanges.emplace_back();
            return static_cast<int>(m_tables.animRanges.size()) - 1;
        case SpriteKind::Body:
            m_tables.bodyRanges.emplace_back();
            return static_cast<int>(m_tables.bodyRanges.size()) - 1;
        case SpriteKind::Sequence:
            m_tables.sequenceRanges.emplace_back();
            return static_cast<int>(m_tables.sequenceRanges.size()) - 1;
        case SpriteKind::Building:
            m_tables.buildings.emplace_back();
            return static_cast<int>(m_tables.buildings.size()) - 1;
        default:
            return -1;
    }
}

void SpriteRegistry::Emit(std::string_view object, const Item &item, int handle)
{
    const std::string where
        = "sprite '" + std::string(object) + "' " + SpriteKindName(item.kind) + " '" + item.name + "'";
    auto &t = m_tables;
    auto childOf = [&](const Entry &e) -> std::int16_t {
        const SpriteRef r = LookupRef(object, e.ref, e.kind, where);
        return static_cast<std::int16_t>(r.handle);
    };
    auto range = [](std::size_t before, std::size_t after) {
        if (after == before)
            return IndexRange {0, -1};
        return IndexRange {static_cast<std::int16_t>(before), static_cast<std::int16_t>(after - 1)};
    };

    switch (item.kind)
    {
        case SpriteKind::Frame:
        {
            const auto img = m_objects.find(object);
            SpriteFrame &f = t.frames[handle];
            const float scale = img != m_objects.end() ? img->second.image.scale : 1.0f;
            f.id = handle;
            f.page = img != m_objects.end() ? img->second.imageIndex : -1;
            f.srcX = item.rect[0];
            f.srcY = item.rect[1];
            f.srcW = item.rect[2];
            f.srcH = item.rect[3];
            f.width = item.size[0] >= 0 ? item.size[0] : item.rect[2] / scale;
            f.height = item.size[1] >= 0 ? item.size[1] : item.rect[3] / scale;
            // In drawing units, as the sheet frames have them (pixels / pixels-per-unit of this frame)
            const float sx = f.width > 0 ? item.rect[2] / f.width : scale;
            const float sy = f.height > 0 ? item.rect[3] / f.height : scale;
            f.x = sx > 0 ? item.rect[0] / sx : 0.0f;
            f.y = sy > 0 ? item.rect[1] / sy : 0.0f;
            f.palette = item.palette;
            f.recolor = static_cast<std::uint8_t>(
                item.recolor >= 0 ? item.recolor
                                  : (img != m_objects.end() ? static_cast<int>(img->second.image.recolor) : 0));
            break;
        }
        case SpriteKind::Part:
        {
            const std::size_t before = t.compositeParts.size();
            for (const Entry &e : item.entries)
                t.compositeParts.push_back(
                    {childOf(e),
                     Narrow<std::int16_t>(e.ox, where, m_errors),
                     Narrow<std::int16_t>(e.oy, where, m_errors),
                     IsCompCode(e.kind)});
            t.compositeRanges[handle] = range(before, t.compositeParts.size());
            break;
        }
        case SpriteKind::Anim:
        {
            const std::size_t before = t.animFrames.size();
            for (const Entry &e : item.entries)
                t.animFrames.push_back(
                    {childOf(e),
                     Narrow<std::int8_t>(e.ox, where, m_errors),
                     Narrow<std::int8_t>(e.oy, where, m_errors),
                     IsCompCode(e.kind)});
            t.animRanges[handle] = range(before, t.animFrames.size());
            break;
        }
        case SpriteKind::Body:
        {
            const std::size_t before = t.bodyLayers.size();
            for (const Entry &e : item.entries)
                t.bodyLayers.push_back(
                    {childOf(e),
                     Narrow<std::int8_t>(e.ox, where, m_errors),
                     Narrow<std::int8_t>(e.oy, where, m_errors),
                     IsCompCode(e.kind),
                     Narrow<std::uint8_t>(e.flag, where, m_errors)});
            t.bodyRanges[handle] = range(before, t.bodyLayers.size());
            break;
        }
        case SpriteKind::Sequence:
        {
            const std::size_t before = t.sequenceSteps.size();
            for (const Entry &e : item.entries)
                t.sequenceSteps.push_back(
                    {childOf(e),
                     Narrow<std::int8_t>(e.ox, where, m_errors),
                     Narrow<std::int8_t>(e.oy, where, m_errors)});
            t.sequenceRanges[handle] = range(before, t.sequenceSteps.size());
            break;
        }
        case SpriteKind::Building:
        {
            const std::size_t before = t.buildingParts.size();
            for (const Entry &e : item.entries)
                t.buildingParts.push_back(
                    {childOf(e),
                     Narrow<std::int16_t>(e.ox, where, m_errors),
                     Narrow<std::int8_t>(e.oy, where, m_errors),
                     IsCompCode(e.kind),
                     Narrow<std::uint8_t>(e.layer, where, m_errors),
                     Narrow<std::uint8_t>(e.state, where, m_errors)});
            BuildingDef &b = t.buildings[handle];
            const IndexRange r = range(before, t.buildingParts.size());
            b.start = r.start;
            b.end = r.end;
            b.width = Narrow<std::uint8_t>(item.width, where, m_errors);
            b.height = Narrow<std::uint8_t>(item.height, where, m_errors);
            b.style = item.style;
            b.tall = item.tall;
            break;
        }
        default:
            break;
    }
}

bool SpriteRegistry::Compile()
{
    m_tables = {};
    m_names.clear();
    const std::size_t errorsBefore = m_errors.size();

    // 1. Images and handles of every item (objects in name order, items in file order)
    for (auto &[name, obj] : m_objects)
    {
        obj.imageIndex = static_cast<int>(m_tables.images.size());
        m_tables.images.push_back(obj.image);
        for (Item &item : obj.items)
        {
            item.handle = Allocate(item.kind);
            m_names[{static_cast<int>(item.kind), item.handle}] = name + "." + item.name;
        }
    }

    // 2. Families: a contiguous block of copies per family
    struct Slot
    {
        std::string object; // the object the copied item belongs to (its references resolve from there)
        const Item *source;
        int handle;
    };
    std::vector<Slot> slots;
    for (auto &[name, obj] : m_objects)
    {
        for (FamilyDef &fam : obj.families)
        {
            fam.compiled = {fam.kind, -1, 0};
            std::vector<std::pair<std::string, const Item *>> members;
            for (const std::string &m : fam.members)
            {
                const Item *item = LookupItem(name, m, fam.kind);
                if (!item)
                {
                    Error(
                        "sprite '" + name + "' family '" + fam.name + "': no " + SpriteKindName(fam.kind) + " '" + m
                        + "'");
                    item = nullptr;
                }
                const auto dot = m.find('.');
                members.emplace_back(dot == std::string::npos ? name : m.substr(0, dot), item);
            }
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const int h = Allocate(fam.kind);
                if (i == 0)
                    fam.compiled.start = h;
                ++fam.compiled.count;
                slots.push_back({members[i].first, members[i].second, h});
            }
        }
    }

    // 3. Contents (references resolve now that every handle, family starts included, is known)
    for (const auto &[name, obj] : m_objects)
        for (const Item &item : obj.items)
            Emit(name, item, item.handle);
    for (const Slot &s : slots)
    {
        if (s.source)
            Emit(s.object, *s.source, s.handle);
    }

    for (std::size_t i = errorsBefore; i < m_errors.size(); ++i)
        std::cerr << "[SpriteRegistry] " << m_errors[i] << std::endl;
    return m_errors.empty();
}

bool SpriteRegistry::HasObject(std::string_view name) const
{
    return m_objects.count(name) != 0;
}

std::vector<std::string> SpriteRegistry::ObjectNames() const
{
    std::vector<std::string> names;
    names.reserve(m_objects.size());
    for (const auto &kv : m_objects)
        names.push_back(kv.first);
    return names;
}

std::vector<std::string> SpriteRegistry::ItemNames(std::string_view object, SpriteKind kind) const
{
    std::vector<std::string> out;
    const auto it = m_objects.find(object);
    if (it != m_objects.end())
        for (const Item &item : it->second.items)
            if (item.kind == kind)
                out.push_back(item.name);
    return out;
}

std::vector<std::string> SpriteRegistry::FamilyNames(std::string_view object) const
{
    std::vector<std::string> out;
    const auto it = m_objects.find(object);
    if (it != m_objects.end())
        for (const FamilyDef &f : it->second.families)
            out.push_back(f.name);
    return out;
}

int SpriteRegistry::ImageOf(std::string_view object) const
{
    const auto it = m_objects.find(object);
    return it == m_objects.end() ? -1 : it->second.imageIndex;
}

SpriteRef SpriteRegistry::Resolve(std::string_view ref, SpriteKind expected) const
{
    const auto dot = ref.find('.');
    if (dot == std::string_view::npos)
        return {};
    const std::string_view objName = ref.substr(0, dot);
    const std::string_view itemName = ref.substr(dot + 1);
    const auto it = m_objects.find(objName);
    if (it == m_objects.end())
        return {};
    for (int k = static_cast<int>(SpriteKind::Frame); k <= static_cast<int>(SpriteKind::Building); ++k)
    {
        if (expected != SpriteKind::None && k != static_cast<int>(expected))
            continue;
        const auto found = it->second.byName[k].find(itemName);
        if (found != it->second.byName[k].end())
            return {static_cast<SpriteKind>(k), it->second.items[found->second].handle};
    }
    return FamilyMember(it->second, itemName, expected);
}

const SpriteFamily *SpriteRegistry::FindFamily(std::string_view object, std::string_view family) const
{
    const auto it = m_objects.find(object);
    if (it == m_objects.end())
        return nullptr;
    const FamilyDef *fam = FindFamilyDef(it->second, family);
    return fam ? &fam->compiled : nullptr;
}

std::string SpriteRegistry::NameOf(SpriteRef ref) const
{
    const auto it = m_names.find({static_cast<int>(ref.kind), ref.handle});
    if (it != m_names.end())
        return it->second;
    // A family member: "<object>.<family>[i]"
    for (const auto &[name, obj] : m_objects)
        for (const FamilyDef &f : obj.families)
            if (f.kind == ref.kind && ref.handle >= f.compiled.start
                && ref.handle < f.compiled.start + f.compiled.count)
                return name + "." + f.name + "[" + std::to_string(ref.handle - f.compiled.start) + "]";
    return {};
}

namespace
{
SpriteRegistry &Instance()
{
    static SpriteRegistry reg;
    return reg;
}
bool g_loaded = false;
int g_generation = 0;
std::set<std::string> g_reported; // unresolved references already reported
} // namespace

SpriteRegistry &Sprites()
{
    if (!g_loaded)
    {
        g_loaded = true;
        ++g_generation;
        Instance().Clear();
        Instance().LoadDirectory(LoaderResources::SpritesDir());
        std::cout << "[SpriteRegistry] " << Instance().ObjectNames().size() << " sprite objects from "
                  << LoaderResources::SpritesDir() << std::endl;
    }
    return Instance();
}

void ReloadSprites()
{
    g_loaded = false;
    g_reported.clear();
    Sprites();
}

int SpritesGeneration()
{
    Sprites();
    return g_generation;
}

int CachedSprite::Get() const
{
    const int gen = SpritesGeneration();
    if (gen != m_generation)
    {
        m_generation = gen;
        m_handle = SpriteHandle(m_ref, m_kind, "code");
    }
    return m_handle;
}

int SpriteHandle(std::string_view ref, SpriteKind kind, std::string_view context)
{
    if (ref.empty())
        return -1;
    const SpriteRef r = Sprites().Resolve(ref, kind);
    if (r.Valid())
        return r.handle;
    if (g_reported.insert(std::string(context) + "|" + std::string(ref)).second)
        std::cerr << "[SpriteRegistry] " << (context.empty() ? std::string() : std::string(context) + ": ") << "no "
                  << SpriteKindName(kind) << " '" << ref << "'" << std::endl;
    return -1;
}

int SpriteHandle(const YAML::Node &node, SpriteKind kind, std::string_view context)
{
    if (!node || node.IsNull())
        return -1;
    if (!node.IsScalar())
    {
        std::cerr << "[SpriteRegistry] " << context << ": a sprite name is expected" << std::endl;
        return -1;
    }
    const std::string v = node.as<std::string>();
    if (!v.empty() && v.find_first_not_of("-0123456789") == std::string::npos)
    {
        if (g_reported.insert(std::string(context) + "|" + v).second)
            std::cerr << "[SpriteRegistry] " << context << ": '" << v
                      << "' is a number of the old sheet sprites; write the sprite name (<object>.<item>)" << std::endl;
        return -1;
    }
    return SpriteHandle(v, kind, context);
}

std::vector<int> SpriteHandles(const YAML::Node &node, SpriteKind kind, std::string_view context)
{
    std::vector<int> out;
    if (!node)
        return out;
    if (node.IsSequence())
    {
        for (const auto &n : node)
        {
            const int h = SpriteHandle(n, kind, context);
            if (h >= 0)
                out.push_back(h);
        }
    }
    else
    {
        const int h = SpriteHandle(node, kind, context);
        if (h >= 0)
            out.push_back(h);
    }
    return out;
}

std::string SpriteName(SpriteKind kind, int handle)
{
    if (handle < 0)
        return {};
    return Sprites().NameOf({kind, handle});
}

} // namespace OpenArtOfWar
