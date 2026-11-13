#include "SpriteMerge.hpp"

#include <algorithm>
#include <random>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <set>
#include <sstream>

namespace CharEditor::SpriteMerge
{

namespace
{

const char *const kSections[] = {"frames", "parts", "anims", "bodies", "sequences", "buildings", "families"};
const char *const kEntryKeys[] = {"frame", "part", "anim", "body"};

bool WordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string Str(const YAML::Node &n, const char *key, std::string_view def)
{
    try
    {
        if (n && n.IsMap() && n[key] && n[key].IsScalar())
            return n[key].as<std::string>();
    }
    catch (...)
    {}
    return std::string(def);
}

float Scale(const YAML::Node &meta)
{
    try
    {
        const float s = meta["scale"] ? meta["scale"].as<float>() : 1.0f;
        return s > 0.0f ? s : 1.0f;
    }
    catch (...)
    {
        return 1.0f;
    }
}

YAML::Node FlowSeq(std::initializer_list<double> values)
{
    YAML::Node n(YAML::NodeType::Sequence);
    n.SetStyle(YAML::EmitterStyle::Flow);
    for (double v : values)
    {
        if (std::fabs(v - std::round(v)) < 1e-4)
            n.push_back(static_cast<long long>(std::llround(v)));
        else
            n.push_back(std::round(v * 10000.0) / 10000.0);
    }
    return n;
}

// The references of entries ({frame: x, at: ...}) anywhere below `n`
void RewriteEntries(YAML::Node n, std::string_view from, const NameMap &names)
{
    if (n.IsSequence())
    {
        for (YAML::Node child : n)
            RewriteEntries(child, from, names);
    }
    else if (n.IsMap())
    {
        for (auto kv : n)
        {
            const std::string key = kv.first.as<std::string>("");
            YAML::Node value = kv.second;
            const bool isRef
                = std::any_of(std::begin(kEntryKeys), std::end(kEntryKeys), [&](const char *k) { return key == k; });
            if (isRef && value.IsScalar())
                value = RewriteMetaRef(value.as<std::string>(), from, names);
            else
                RewriteEntries(value, from, names);
        }
    }
}

// The members of families: { idle: { sequences: [a, b] } }
void RewriteFamily(YAML::Node fam, std::string_view from, const NameMap &names)
{
    if (!fam.IsMap())
        return;
    for (auto kv : fam)
    {
        YAML::Node list = kv.second;
        if (!list.IsSequence())
            continue;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].IsScalar())
                list[i] = RewriteMetaRef(list[i].as<std::string>(), from, names);
    }
}

} // namespace

void FitMetaToImage(YAML::Node meta, int w, int h)
{
    const YAML::Node d = meta["image_size"];
    if (w <= 0 || h <= 0)
        return;
    const float dw = d && d.IsSequence() && d.size() == 2 ? d[0].as<float>(0.0f) : 0.0f;
    const float dh = d && d.IsSequence() && d.size() == 2 ? d[1].as<float>(0.0f) : 0.0f;
    if (dw > 0.0f && dh > 0.0f && (std::lround(dw) != w || std::lround(dh) != h))
    {
        const float kx = w / dw, ky = h / dh;
        const bool even = std::fabs(kx - ky) < 1e-3f;
        const float scale = Scale(meta);
        if (even)
        {
            const float ns = scale * kx;
            if (std::fabs(ns - std::round(ns)) < 1e-4f)
                meta["scale"] = static_cast<long long>(std::llround(ns));
            else
                meta["scale"] = ns;
        }
        if (YAML::Node frames = meta["frames"]; frames && frames.IsMap())
            for (auto kv : frames)
            {
                YAML::Node v = kv.second;
                YAML::Node rect = v.IsMap() ? v["rect"] : v;
                if (!rect || !rect.IsSequence() || rect.size() < 4)
                    continue;
                const double r[4]
                    = {rect[0].as<double>(0), rect[1].as<double>(0), rect[2].as<double>(1), rect[3].as<double>(1)};
                YAML::Node nr = FlowSeq(
                    {std::round(r[0] * kx),
                     std::round(r[1] * ky),
                     std::max(1.0, std::round(r[2] * kx)),
                     std::max(1.0, std::round(r[3] * ky))});
                if (v.IsMap())
                {
                    v["rect"] = nr;
                    if (!even && !v["size"])
                        v["size"] = FlowSeq({r[2] / scale, r[3] / scale});
                }
                else if (even)
                {
                    frames[kv.first] = nr;
                }
                else
                {
                    YAML::Node f(YAML::NodeType::Map);
                    f.SetStyle(YAML::EmitterStyle::Flow);
                    f["rect"] = nr;
                    f["size"] = FlowSeq({r[2] / scale, r[3] / scale});
                    frames[kv.first] = f;
                }
            }
    }
    YAML::Node size = FlowSeq({double(w), double(h)});
    meta["image_size"] = size;
}

std::vector<std::string> DefinedNames(const YAML::Node &meta)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const char *sec : kSections)
    {
        const YAML::Node s = meta[sec];
        if (!s || !s.IsMap())
            continue;
        for (const auto &kv : s)
        {
            const std::string n = kv.first.as<std::string>("");
            if (!n.empty() && seen.insert(n).second)
                out.push_back(n);
        }
    }
    return out;
}

NameMap BuildNameMap(const std::vector<Source> &sources)
{
    NameMap map;
    std::set<std::string> taken;
    for (std::size_t s = 0; s < sources.size(); ++s)
    {
        const std::vector<std::string> own = DefinedNames(sources[s].meta);
        const std::set<std::string> ownSet(own.begin(), own.end());
        auto &m = map[sources[s].object];
        std::set<std::string> added;
        for (const std::string &n : own)
        {
            std::string target = n;
            if (taken.count(n))
            {
                const std::string base = sources[s].object + "_" + n;
                target = base;
                for (int i = 2; taken.count(target) || added.count(target) || (ownSet.count(target) && target != n);
                     ++i)
                    target = base + std::to_string(i);
            }
            m[n] = target;
            added.insert(target);
        }
        taken.insert(added.begin(), added.end());
    }
    return map;
}

std::string RewriteMetaRef(std::string_view ref, std::string_view fromObject, const NameMap &names)
{
    const auto dot = ref.find('.');
    const std::string object
        = dot == std::string_view::npos ? std::string(fromObject) : std::string(ref.substr(0, dot));
    std::string name = dot == std::string_view::npos ? std::string(ref) : std::string(ref.substr(dot + 1));
    const auto it = names.find(object);
    if (it == names.end())
        return std::string(ref); // another object: unchanged
    std::string suffix;
    const auto open = name.find('[');
    if (open != std::string::npos)
    {
        suffix = name.substr(open);
        name = name.substr(0, open);
    }
    const auto found = it->second.find(name);
    return (found == it->second.end() ? name : found->second) + suffix;
}

YAML::Node BuildMergedFrameNode(
    const YAML::Node &srcVal,
    const std::array<int, 2> &place,
    float srcScale,
    float dstScale,
    std::string_view srcRecolor,
    std::string_view dstRecolor)
{
    const YAML::Node rect = srcVal.IsMap() ? srcVal["rect"] : srcVal;
    double w = 1, h = 1;
    if (rect && rect.IsSequence() && rect.size() >= 4)
    {
        w = rect[2].as<double>(1.0);
        h = rect[3].as<double>(1.0);
    }
    YAML::Node f(YAML::NodeType::Map);
    f.SetStyle(YAML::EmitterStyle::Flow);
    f["rect"] = FlowSeq({double(place[0]), double(place[1]), w, h});
    if (srcVal.IsMap() && srcVal["size"])
        f["size"] = srcVal["size"];
    else if (std::fabs(srcScale - dstScale) > 1e-4f)
        f["size"] = FlowSeq({w / srcScale, h / srcScale});
    const std::string fr = srcVal.IsMap() ? Str(srcVal, "recolor", srcRecolor) : std::string(srcRecolor);
    if (fr != dstRecolor)
        f["recolor"] = fr;
    if (srcVal.IsMap())
        for (const auto &e : srcVal)
        {
            const std::string k = e.first.as<std::string>("");
            if (k != "rect" && k != "size" && k != "recolor")
                f[k] = e.second;
        }
    return f;
}

YAML::Node MergeMetas(
    const std::vector<Source> &sources,
    const NameMap &names,
    const FramePlaces &places,
    std::string_view image,
    std::string_view resistanceImage,
    int imageW,
    int imageH)
{
    YAML::Node out(YAML::NodeType::Map);
    if (sources.empty())
        return out;
    const YAML::Node &first = sources.front().meta;
    const float scale = Scale(first);
    const std::string recolor = Str(first, "recolor", "none");
    out["image"] = std::string(image);
    out["image_size"] = FlowSeq({double(imageW), double(imageH)});
    if (!resistanceImage.empty())
        out["resistance_image"] = std::string(resistanceImage);
    if (std::fabs(scale - std::round(scale)) < 1e-4f)
        out["scale"] = static_cast<long long>(std::llround(scale));
    else
        out["scale"] = scale;
    if (first["terrain"] && first["terrain"].as<bool>(false))
        out["terrain"] = true;
    if (recolor != "none")
        out["recolor"] = recolor;

    for (const char *sec : kSections)
    {
        YAML::Node section(YAML::NodeType::Map);
        for (const Source &src : sources)
        {
            const YAML::Node s = src.meta[sec];
            if (!s || !s.IsMap())
                continue;
            const auto &m = names.at(src.object);
            const float srcScale = Scale(src.meta);
            const std::string srcRecolor = Str(src.meta, "recolor", "none");
            for (const auto &kv : s)
            {
                const std::string name = kv.first.as<std::string>("");
                const std::string target = m.count(name) ? m.at(name) : name;
                YAML::Node value = YAML::Clone(kv.second);
                if (std::string(sec) == "frames")
                {
                    const auto place = places.find({src.object, name});
                    if (place == places.end())
                        continue;
                    value = BuildMergedFrameNode(kv.second, place->second, srcScale, scale, srcRecolor, recolor);
                }
                else if (std::string(sec) == "families")
                {
                    RewriteFamily(value, src.object, names);
                }
                else
                {
                    RewriteEntries(value, src.object, names);
                }
                section[target] = value;
            }
        }
        if (section.size())
            out[sec] = section;
    }
    return out;
}

std::string EmitMeta(const YAML::Node &meta, std::string_view header)
{
    YAML::Emitter e;
    e.SetIndent(2);
    e.SetSeqFormat(YAML::Block);
    e << meta;
    std::string text = std::string(header) + e.c_str();
    if (text.empty() || text.back() != '\n')
        text += '\n';
    return text;
}

// Note: qualifyObject non-empty qualifies local references belonging to that object (used during SplitMeta)
std::string RewriteRefWithMoved(
    std::string_view ref,
    std::string_view currentObject,
    const std::set<std::string> &movedItems,
    std::string_view destObject,
    bool rewriteMoved)
{
    const auto dot = ref.find('.');
    const std::string object
        = dot == std::string_view::npos ? std::string(currentObject) : std::string(ref.substr(0, dot));
    std::string name = dot == std::string_view::npos ? std::string(ref) : std::string(ref.substr(dot + 1));
    std::string suffix;
    const auto open = name.find('[');
    if (open != std::string::npos)
    {
        suffix = name.substr(open);
        name = name.substr(0, open);
    }
    if (object != currentObject)
        return std::string(ref);
    const bool isMoved = movedItems.count(name) > 0;
    if (isMoved == rewriteMoved)
        return std::string(destObject) + "." + name + suffix;
    return name + suffix;
}

void RewriteSplitEntries(
    YAML::Node n,
    std::string_view currentObject,
    const std::set<std::string> &movedItems,
    std::string_view destObject,
    bool rewriteMoved)
{
    if (n.IsSequence())
    {
        for (YAML::Node child : n)
            RewriteSplitEntries(child, currentObject, movedItems, destObject, rewriteMoved);
    }
    else if (n.IsMap())
    {
        for (auto kv : n)
        {
            const std::string key = kv.first.as<std::string>("");
            YAML::Node value = kv.second;
            const bool isRef
                = std::any_of(std::begin(kEntryKeys), std::end(kEntryKeys), [&](const char *k) { return key == k; });
            if (isRef && value.IsScalar())
                value = RewriteRefWithMoved(value.as<std::string>(), currentObject, movedItems, destObject, rewriteMoved);
            else
                RewriteSplitEntries(value, currentObject, movedItems, destObject, rewriteMoved);
        }
    }
}

void RewriteSplitFamily(
    YAML::Node fam,
    std::string_view currentObject,
    const std::set<std::string> &movedItems,
    std::string_view destObject,
    bool rewriteMoved)
{
    if (!fam.IsMap())
        return;
    for (auto kv : fam)
    {
        YAML::Node list = kv.second;
        if (!list.IsSequence())
            continue;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].IsScalar())
                list[i] = RewriteRefWithMoved(
                    list[i].as<std::string>(), currentObject, movedItems, destObject, rewriteMoved);
    }
}

void CollectLocalRefs(
    const YAML::Node &n, std::string_view srcObject, std::vector<std::string> &out, bool isFamily = false)
{
    if (!n)
        return;
    if (isFamily && n.IsMap())
    {
        for (const auto &kv : n)
            if (kv.second.IsSequence())
                for (const auto &elem : kv.second)
                    if (elem.IsScalar())
                        CollectLocalRefs(elem, srcObject, out, false);
        return;
    }
    if (n.IsScalar())
    {
        std::string r = n.as<std::string>("");
        const auto dot = r.find('.');
        if (dot != std::string::npos)
        {
            if (r.substr(0, dot) != srcObject)
                return;
            r = r.substr(dot + 1);
        }
        if (const auto open = r.find('['); open != std::string::npos)
            r = r.substr(0, open);
        if (!r.empty())
            out.push_back(r);
        return;
    }
    if (n.IsSequence())
    {
        for (const auto &c : n)
            CollectLocalRefs(c, srcObject, out, false);
    }
    else if (n.IsMap())
    {
        for (const auto &kv : n)
        {
            const std::string key = kv.first.as<std::string>("");
            const bool isRef
                = std::any_of(std::begin(kEntryKeys), std::end(kEntryKeys), [&](const char *k) { return key == k; });
            if (isRef && kv.second.IsScalar())
                CollectLocalRefs(kv.second, srcObject, out, false);
            else if (key == "parts")
                CollectLocalRefs(kv.second, srcObject, out, false);
        }
    }
}

std::string RewriteText(
    std::string_view text, const NameMap &names, std::string_view merged, int &count, bool rewriteBare)
{
    count = 0;
    std::string out;
    out.reserve(text.size());
    std::istringstream in {std::string(text)};
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line))
    {
        if (!firstLine)
            out += '\n';
        firstLine = false;
        // A whole line "sprite: <source>"
        {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '-'))
                ++i;
            for (const char *key : {"sprite:", "reinforced_sprite:"})
            {
                const std::string k = key;
                if (line.compare(i, k.size(), k) != 0)
                    continue;
                std::size_t v = i + k.size();
                while (v < line.size() && line[v] == ' ')
                    ++v;
                std::size_t end = line.size();
                while (end > v && (line[end - 1] == ' ' || line[end - 1] == '\r'))
                    --end;
                std::string value = line.substr(v, end - v);
                std::string quote;
                if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')
                    && value.back() == value.front())
                {
                    quote = value.substr(0, 1);
                    value = value.substr(1, value.size() - 2);
                }
                if (rewriteBare && names.count(value))
                {
                    line = line.substr(0, v) + quote + std::string(merged) + quote + line.substr(end);
                    ++count;
                }
            }
        }
        // "<source>.<item>" anywhere
        std::string res;
        std::size_t p = 0;
        while (p < line.size())
        {
            if (!WordChar(line[p]) || (p > 0 && (WordChar(line[p - 1]) || line[p - 1] == '.')))
            {
                res += line[p++];
                continue;
            }
            std::size_t e = p;
            while (e < line.size() && WordChar(line[e]))
                ++e;
            const std::string word = line.substr(p, e - p);
            const auto obj = names.find(word);
            if (obj != names.end() && e < line.size() && line[e] == '.')
            {
                std::size_t n = e + 1;
                while (n < line.size() && WordChar(line[n]))
                    ++n;
                const auto item = obj->second.find(line.substr(e + 1, n - e - 1));
                if (item != obj->second.end())
                {
                    res += std::string(merged) + "." + item->second;
                    ++count;
                    p = n;
                    continue;
                }
            }
            res += word;
            p = e;
        }
        out += res;
    }
    if (!text.empty() && text.back() == '\n')
        out += '\n';
    return out;
}

SplitResult SplitMeta(
    std::string_view srcObject,
    const YAML::Node &srcMeta,
    const std::set<std::string> &movedFrames,
    const FramePlaces &newPlaces,
    std::string_view newObject,
    std::string_view newImage,
    std::string_view newResistanceImage,
    int newW,
    int newH)
{
    SplitResult res;
    res.remainingSourceMeta = YAML::Clone(srcMeta);
    res.newObjectMeta = YAML::Node(YAML::NodeType::Map);
    res.movedItems = movedFrames;

    std::set<std::string> allLocal;
    for (const std::string &n : DefinedNames(srcMeta))
        allLocal.insert(n);

    // Transitive closure: an item belongs exclusively to the moved set when all of its local refs are moved
    std::map<std::string, std::set<std::string>> movedBySection;
    movedBySection["frames"] = movedFrames;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (std::size_t si = 1; si < std::size(kSections); ++si)
        {
            const char *sec = kSections[si];
            const YAML::Node s = srcMeta[sec];
            if (!s || !s.IsMap())
                continue;
            const bool isFam = std::string_view(sec) == "families";
            for (const auto &kv : s)
            {
                const std::string itemName = kv.first.as<std::string>("");
                if (itemName.empty() || movedBySection[sec].count(itemName))
                    continue;
                std::vector<std::string> refs;
                CollectLocalRefs(kv.second, srcObject, refs, isFam);
                bool usesMoved = false;
                bool usesStaying = false;
                for (const std::string &r : refs)
                {
                    if (res.movedItems.count(r))
                        usesMoved = true;
                    else if (allLocal.count(r))
                        usesStaying = true;
                }
                if (usesMoved && !usesStaying)
                {
                    movedBySection[sec].insert(itemName);
                    res.movedItems.insert(itemName);
                    changed = true;
                }
            }
        }
    }

    const float scale = Scale(srcMeta);
    const std::string recolor = Str(srcMeta, "recolor", "none");
    res.newObjectMeta["image"] = std::string(newImage);
    res.newObjectMeta["image_size"] = FlowSeq({double(newW), double(newH)});
    if (!newResistanceImage.empty())
        res.newObjectMeta["resistance_image"] = std::string(newResistanceImage);
    if (srcMeta["scale"])
        res.newObjectMeta["scale"] = srcMeta["scale"];
    if (srcMeta["terrain"] && srcMeta["terrain"].as<bool>(false))
        res.newObjectMeta["terrain"] = true;
    if (recolor != "none")
        res.newObjectMeta["recolor"] = recolor;

    for (const char *sec : kSections)
    {
        const YAML::Node srcSec = srcMeta[sec];
        if (!srcSec || !srcSec.IsMap())
            continue;
        YAML::Node remSec(YAML::NodeType::Map);
        YAML::Node dstSec(YAML::NodeType::Map);
        const bool isFrames = std::string_view(sec) == "frames";
        const bool isFam = std::string_view(sec) == "families";
        const auto &movedSet = movedBySection[sec];
        for (const auto &kv : srcSec)
        {
            const std::string itemName = kv.first.as<std::string>("");
            if (movedSet.count(itemName))
            {
                YAML::Node val = YAML::Clone(kv.second);
                if (isFrames)
                {
                    const auto pl = newPlaces.find({std::string(srcObject), itemName});
                    if (pl != newPlaces.end())
                        val = BuildMergedFrameNode(val, pl->second, scale, scale, recolor, recolor);
                }
                else if (isFam)
                {
                    RewriteSplitFamily(val, srcObject, res.movedItems, srcObject, false);
                }
                else
                {
                    RewriteSplitEntries(val, srcObject, res.movedItems, srcObject, false);
                }
                dstSec[itemName] = val;
            }
            else
            {
                YAML::Node val = YAML::Clone(kv.second);
                if (isFam)
                    RewriteSplitFamily(val, srcObject, res.movedItems, newObject, true);
                else if (!isFrames)
                    RewriteSplitEntries(val, srcObject, res.movedItems, newObject, true);
                remSec[itemName] = val;
            }
        }
        if (remSec.size())
            res.remainingSourceMeta[sec] = remSec;
        else
            res.remainingSourceMeta.remove(sec);
        if (dstSec.size())
            res.newObjectMeta[sec] = dstSec;
    }

    res.movedEverything = true;
    for (const char *sec : kSections)
    {
        if (res.remainingSourceMeta[sec] && res.remainingSourceMeta[sec].size() > 0)
        {
            res.movedEverything = false;
            break;
        }
    }
    return res;
}

bool BindsBare(std::string_view text, std::string_view object)
{
    std::istringstream in {std::string(text)};
    std::string line;
    while (std::getline(in, line))
    {
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ')
            ++i;
        if (line.compare(i, 7, "sprite:") != 0)
            continue;
        std::string v = line.substr(i + 7);
        v.erase(0, v.find_first_not_of(" \"'"));
        v.erase(v.find_last_not_of(" \"'\r") + 1);
        if (v == object)
            return true;
    }
    return false;
}

namespace
{
// Fills rows in the given order; the row width follows the total area
std::vector<std::array<int, 2>> RowPack(
    const std::vector<std::array<int, 2>> &sizes, const std::vector<std::size_t> &order, int padding)
{
    std::vector<std::array<int, 2>> out(sizes.size(), {0, 0});
    double area = 0;
    int widest = 0;
    for (const auto &s : sizes)
    {
        area += double(s[0] + padding) * (s[1] + padding);
        widest = std::max(widest, s[0]);
    }
    const int rowWidth = std::max(widest, static_cast<int>(std::ceil(std::sqrt(area) * 1.15)));
    int x = 0, y = 0, rowH = 0;
    for (std::size_t i : order)
    {
        if (x > 0 && x + sizes[i][0] > rowWidth)
        {
            x = 0;
            y += rowH + padding;
            rowH = 0;
        }
        out[i] = {x, y};
        x += sizes[i][0] + padding;
        rowH = std::max(rowH, sizes[i][1]);
    }
    return out;
}
} // namespace

std::vector<std::array<int, 2>> ShelfPack(const std::vector<std::array<int, 2>> &sizes, int padding)
{
    std::vector<std::size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return sizes[a][1] != sizes[b][1] ? sizes[a][1] > sizes[b][1] : sizes[a][0] > sizes[b][0];
    });
    return RowPack(sizes, order, padding);
}

std::vector<std::array<int, 2>> ShufflePack(const std::vector<std::array<int, 2>> &sizes, int padding, unsigned seed)
{
    std::vector<std::size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    return RowPack(sizes, order, padding);
}

std::vector<std::array<int, 2>> GridPack(const std::vector<std::array<int, 2>> &sizes, int padding)
{
    std::vector<std::array<int, 2>> out(sizes.size(), {0, 0});
    if (sizes.empty())
        return out;
    int cellW = 1, cellH = 1;
    for (const auto &s : sizes)
    {
        cellW = std::max(cellW, s[0]);
        cellH = std::max(cellH, s[1]);
    }
    // As square as the cells allow
    const double n = static_cast<double>(sizes.size());
    const int cols = std::max(
        1, static_cast<int>(std::lround(std::sqrt(n * (cellH + padding) / static_cast<double>(cellW + padding)))));
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
        out[i] = {
            col * (cellW + padding) + (cellW - sizes[i][0]) / 2, row * (cellH + padding) + (cellH - sizes[i][1]) / 2};
    }
    return out;
}

std::array<int, 2> PackExtent(const std::vector<std::array<int, 2>> &sizes, const std::vector<std::array<int, 2>> &pos)
{
    std::array<int, 2> e {1, 1};
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        e[0] = std::max(e[0], pos[i][0] + sizes[i][0]);
        e[1] = std::max(e[1], pos[i][1] + sizes[i][1]);
    }
    return e;
}

std::array<int, 2> FitToLongestSide(int w, int h, int n)
{
    w = std::max(1, w);
    h = std::max(1, h);
    n = std::max(1, n);
    if (w >= h)
        return {n, std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * n / w)))};
    return {std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * n / h))), n};
}

std::array<int, 2> CustomSize(int w, int h, int newW, int newH, bool keepAspect, bool changedW)
{
    w = std::max(1, w);
    h = std::max(1, h);
    newW = std::max(1, newW);
    newH = std::max(1, newH);
    if (keepAspect)
    {
        if (changedW)
            newH = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * newW / w)));
        else
            newW = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * newH / h)));
    }
    return {newW, newH};
}

int DetectIntegerUpscale(const unsigned char *rgba, int w, int h, const std::vector<std::array<int, 4>> &rects)
{
    if (!rgba || w <= 0 || h <= 0)
        return 1;
    auto px = [&](int x, int y) {
        std::uint32_t v;
        std::memcpy(&v, rgba + (static_cast<std::size_t>(y) * w + x) * 4, 4);
        return v;
    };
    int g = std::gcd(w, h);
    // Runs of equal pixels along rows and columns all start on the grid: their lengths are multiples of k
    for (int y = 0; y < h && g > 1; ++y)
    {
        int run = 1;
        for (int x = 1; x <= w; ++x)
        {
            if (x < w && px(x, y) == px(x - 1, y))
            {
                ++run;
                continue;
            }
            g = std::gcd(g, run);
            run = 1;
            if (g == 1)
                break;
        }
    }
    for (int x = 0; x < w && g > 1; ++x)
    {
        int run = 1;
        for (int y = 1; y <= h; ++y)
        {
            if (y < h && px(x, y) == px(x, y - 1))
            {
                ++run;
                continue;
            }
            g = std::gcd(g, run);
            run = 1;
            if (g == 1)
                break;
        }
    }
    for (const auto &r : rects)
        for (int v : r)
            g = std::gcd(g, std::abs(v));
    return std::max(1, g);
}

std::vector<unsigned char> DownscaleInteger(const unsigned char *rgba, int w, int h, int k)
{
    k = std::max(1, k);
    const int nw = std::max(1, w / k), nh = std::max(1, h / k);
    std::vector<unsigned char> out(static_cast<std::size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
            std::memcpy(
                &out[(static_cast<std::size_t>(y) * nw + x) * 4],
                rgba + (static_cast<std::size_t>(y) * k * w + static_cast<std::size_t>(x) * k) * 4,
                4);
    return out;
}

int SnapDelta(int a0, int a1, const std::vector<int> &targets, int threshold, int *snappedTo)
{
    int best = 0, bestDist = threshold + 1, bestTarget = 0;
    for (int t : targets)
        for (int edge : {a0, a1})
        {
            const int d = std::abs(t - edge);
            if (d < bestDist)
            {
                bestDist = d;
                best = t - edge;
                bestTarget = t;
            }
        }
    if (bestDist > threshold)
        return 0;
    if (snappedTo)
        *snappedTo = bestTarget;
    return best;
}

namespace {

/// Area-weighted median scale across `(value, weight)` pairs; falls back to `fallback` when empty.
double WeightedMedianScale(std::vector<std::pair<double, double>> items, double fallback)
{
    if (items.empty())
        return fallback;
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    double total = 0.0;
    for (const auto &it : items)
        total += it.second;
    if (total <= 0.0)
        return items[items.size() / 2].first;
    double acc = 0.0;
    for (const auto &it : items)
    {
        acc += it.second;
        if (acc >= total * 0.5)
            return it.first;
    }
    return items.back().first;
}

/// Safe integer read for YAML filter keys like `state` / `flag`.
int NodeIntOr(const YAML::Node &n, int fallback)
{
    if (!n || !n.IsScalar())
        return fallback;
    try
    {
        return n.as<int>();
    }
    catch (const std::exception &)
    {
        return fallback;
    }
}

} // namespace

std::vector<AssemblyGroup> AssemblyGroupsFromMeta(
    const YAML::Node &meta, std::string_view object, const std::map<std::string, int> &frameIndex, float scale)
{
    std::vector<AssemblyGroup> groups;
    const YAML::Node frames = meta["frames"], parts = meta["parts"], anims = meta["anims"], families = meta["families"];

    // Measure actual pixels-per-unit from each frame's `rect` and `size`; many metas have a stale header `scale`.
    std::map<int, std::array<double, 2>> framePpu;
    std::map<int, double> frameArea;
    std::vector<std::pair<double, double>> allSx, allSy;
    if (frames && frames.IsMap())
    {
        for (const auto &kv : frames)
        {
            const auto idxIt = frameIndex.find(kv.first.as<std::string>(""));
            if (idxIt == frameIndex.end() || !kv.second.IsMap())
                continue;
            const YAML::Node r = kv.second["rect"];
            const YAML::Node s = kv.second["size"];
            if (!r || !r.IsSequence() || r.size() != 4 || !s || !s.IsSequence() || s.size() != 2)
                continue;
            try
            {
                const double rw = r[2].as<double>();
                const double rh = r[3].as<double>();
                const double sw = s[0].as<double>();
                const double sh = s[1].as<double>();
                if (rw > 0.0 && rh > 0.0 && sw > 0.0 && sh > 0.0)
                {
                    const double px = rw / sw;
                    const double py = rh / sh;
                    const double area = rw * rh;
                    framePpu[idxIt->second] = {px, py};
                    frameArea[idxIt->second] = area;
                    allSx.push_back({px, area});
                    allSy.push_back({py, area});
                }
            }
            catch (const std::exception &)
            {}
        }
    }
    const double fallbackScale = scale > 0.0f ? static_cast<double>(scale) : 1.0;
    const double objSx = WeightedMedianScale(std::move(allSx), fallbackScale);
    const double objSy = WeightedMedianScale(std::move(allSy), fallbackScale);

    // "object.item[i]" / "item[i]" -> {item, i} for this object; another object's name is not ours (empty)
    auto local = [&](std::string name, int &index) -> std::string {
        index = 0;
        const std::string prefix = std::string(object) + ".";
        if (name.rfind(prefix, 0) == 0)
            name = name.substr(prefix.size());
        if (name.find('.') != std::string::npos)
            return {};
        if (const auto open = name.find('['); open != std::string::npos)
        {
            index = std::atoi(name.c_str() + open + 1);
            name = name.substr(0, open);
        }
        return name;
    };
    // A family member by kind ("frames" / "parts" / "anims"): the name of its index-th member
    auto member = [&](std::string_view family, const char *kind, int index) -> std::string {
        if (!families || !families.IsMap())
            return {};
        const YAML::Node f = families[std::string(family)];
        if (!f || !f.IsMap() || !f[kind] || !f[kind].IsSequence() || index < 0
            || index >= static_cast<int>(f[kind].size()))
            return {};
        return f[kind][index].as<std::string>();
    };

    struct RawPiece
    {
        int frame;
        double wx;
        double wy;
    };
    std::vector<RawPiece> rawGroup;
    std::function<void(const YAML::Node &, double, double, int, const char *, int)> walk;
    // One entry of a composite: a frame where it is drawn, a part / the first picture of an animation opened up
    auto entry = [&](const YAML::Node &e, double ox, double oy, int depth) {
        if (!e.IsMap())
            return;
        double ax = ox, ay = oy;
        if (const YAML::Node at = e["at"]; at && at.IsSequence() && at.size() == 2)
        {
            ax += at[0].as<double>();
            ay += at[1].as<double>();
        }
        int index = 0;
        if (const YAML::Node f = e["frame"])
        {
            std::string name = local(f.as<std::string>(), index);
            if (!name.empty() && !(frames && frames[name]))
                name = member(name, "frames", index);
            const auto it = frameIndex.find(name);
            if (it != frameIndex.end())
                rawGroup.push_back({it->second, ax, ay});
        }
        else if (const YAML::Node p = e["part"]; p && depth < 6)
        {
            std::string name = local(p.as<std::string>(), index);
            if (!name.empty() && !(parts && parts[name]))
                name = member(name, "parts", index);
            if (!name.empty() && parts && parts[name])
                walk(parts[name], ax, ay, depth + 1, nullptr, 0);
        }
        else if (const YAML::Node a = e["anim"]; a && depth < 6)
        {
            std::string name = local(a.as<std::string>(), index);
            if (!name.empty() && !(anims && anims[name]))
                name = member(name, "anims", index);
            if (!name.empty() && anims && anims[name] && anims[name].IsSequence() && anims[name].size() > 0)
            {
                YAML::Node first = YAML::Node(YAML::NodeType::Sequence);
                first.push_back(anims[name][0]); // the picture the game shows first
                walk(first, ax, ay, depth + 1, nullptr, 0);
            }
        }
    };
    walk = [&](const YAML::Node &list, double ox, double oy, int depth, const char *filterKey, int baseMax) {
        if (!list.IsSequence())
            return;
        // Two-pass order when `filterKey` is set: base pieces (`val <= baseMax`) before overlays (`val > baseMax`).
        const int passes = filterKey ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass)
        {
            for (const auto &e : list)
            {
                try
                {
                    if (filterKey && e.IsMap())
                    {
                        const int v = NodeIntOr(e[filterKey], 0);
                        if ((pass == 0 && v > baseMax) || (pass == 1 && v <= baseMax))
                            continue;
                    }
                    else if (filterKey && pass == 1)
                        continue;
                    entry(e, ox, oy, depth);
                }
                catch (const std::exception &)
                {}
            }
        }
    };
    auto close = [&]() {
        if (rawGroup.empty())
            return;
        std::vector<std::pair<double, double>> gSx, gSy;
        for (const RawPiece &rp : rawGroup)
        {
            if (const auto it = framePpu.find(rp.frame); it != framePpu.end())
            {
                const double w = frameArea.count(rp.frame) ? frameArea[rp.frame] : 1.0;
                gSx.push_back({it->second[0], w});
                gSy.push_back({it->second[1], w});
            }
        }
        const double sx = WeightedMedianScale(std::move(gSx), objSx);
        const double sy = WeightedMedianScale(std::move(gSy), objSy);
        AssemblyGroup group;
        group.reserve(rawGroup.size());
        for (const RawPiece &rp : rawGroup)
            group.push_back({rp.frame, static_cast<int>(std::lround(rp.wx * sx)), static_cast<int>(std::lround(rp.wy * sy))});
        groups.push_back(std::move(group));
        rawGroup.clear();
    };
    // What the game draws as a whole first: a unit body (hull, turret, shadow...), a building with all its parts
    if (const YAML::Node bodies = meta["bodies"]; bodies && bodies.IsMap())
        for (const auto &kv : bodies)
        {
            walk(kv.second, 0.0, 0.0, 0, "flag", 1);
            close();
        }
    if (const YAML::Node buildings = meta["buildings"]; buildings && buildings.IsMap())
        for (const auto &kv : buildings)
        {
            if (kv.second.IsMap())
                walk(kv.second["parts"], 0.0, 0.0, 0, "state", 0);
            close();
        }
    // Then standalone composites (parts, anims) for frames no body / building claims
    if (parts && parts.IsMap())
        for (const auto &kv : parts)
        {
            walk(kv.second, 0.0, 0.0, 0, nullptr, 0);
            close();
        }
    if (anims && anims.IsMap())
        for (const auto &kv : anims)
        {
            walk(kv.second, 0.0, 0.0, 0, nullptr, 0);
            close();
        }
    return groups;
}

std::vector<std::array<int, 2>> AssemblePack(
    const std::vector<std::array<int, 2>> &sizes, const std::vector<AssemblyGroup> &groups, int padding)
{
    struct Block
    {
        std::vector<AssemblyPiece> pieces; // offsets from the block's top left
        int w {1}, h {1};
    };
    std::vector<Block> blocks;
    std::vector<char> placed(sizes.size(), 0);
    // Two frames in the image must not share pixels; one that does not touch another keeps `padding` from it only when
    // it had to be moved
    auto hits = [&](const AssemblyPiece &a, const AssemblyPiece &b, int gap) {
        return a.x < b.x + sizes[b.frame][0] + gap && b.x < a.x + sizes[a.frame][0] + gap
               && a.y < b.y + sizes[b.frame][1] + gap && b.y < a.y + sizes[a.frame][1] + gap;
    };
    for (const AssemblyGroup &g : groups)
    {
        std::vector<AssemblyPiece> mine;
        for (const AssemblyPiece &p : g)
        {
            if (p.frame < 0 || p.frame >= static_cast<int>(sizes.size()) || placed[p.frame])
                continue;
            placed[p.frame] = 1;
            AssemblyPiece q = p;
            auto free = [&](const AssemblyPiece &c, int gap) {
                for (const AssemblyPiece &o : mine)
                    if (hits(c, o, gap))
                        return false;
                return true;
            };
            if (!free(q, 0))
            {
                bool found = false;
                // Check whether all collisions are tiny seam overlaps (`<= 3px` from rounding `at * scale`).
                bool seamOnly = true;
                for (const AssemblyPiece &o : mine)
                {
                    if (!hits(q, o, 0))
                        continue;
                    const int ovX = std::min(q.x + sizes[q.frame][0], o.x + sizes[o.frame][0]) - std::max(q.x, o.x);
                    const int ovY = std::min(q.y + sizes[q.frame][1], o.y + sizes[o.frame][1]) - std::max(q.y, o.y);
                    const int minEdge = std::min(sizes[q.frame][0], sizes[q.frame][1]);
                    if (std::min(ovX, ovY) > 3 || std::min(ovX, ovY) >= std::max(1, minEdge / 2))
                    {
                        seamOnly = false;
                        break;
                    }
                }
                if (seamOnly)
                {
                    // Nudge only along X or Y with zero gap so edge-to-edge mosaic tiles stay aligned.
                    for (int d = 1; d <= 6 && !found; ++d)
                    {
                        const std::array<std::array<int, 2>, 4> dirs {{{d, 0}, {-d, 0}, {0, d}, {0, -d}}};
                        for (const auto &dir : dirs)
                        {
                            AssemblyPiece c {q.frame, p.x + dir[0], p.y + dir[1]};
                            if (free(c, 0))
                            {
                                q = c;
                                found = true;
                                break;
                            }
                        }
                    }
                }
                // A layer drawn over another one (a turret over its hull): the nearest free place around where the
                // game draws it, ring by ring
                for (int r = 1; !found && r < 1 << 14; ++r)
                {
                    AssemblyPiece best = q;
                    long long bestD = -1;
                    for (int oy = -r; oy <= r; ++oy)
                        for (int ox = -r; ox <= r; ++ox)
                        {
                            if (std::max(std::abs(ox), std::abs(oy)) != r)
                                continue;
                            AssemblyPiece c {q.frame, p.x + ox, p.y + oy};
                            if (!free(c, padding))
                                continue;
                            const long long d = static_cast<long long>(ox) * ox + static_cast<long long>(oy) * oy;
                            if (bestD < 0 || d < bestD)
                            {
                                bestD = d;
                                best = c;
                            }
                        }
                    if (bestD >= 0)
                    {
                        q = best;
                        found = true;
                    }
                }
            }
            mine.push_back(q);
        }
        if (mine.empty())
            continue;
        int minX = mine[0].x, minY = mine[0].y, maxX = minX, maxY = minY;
        for (const AssemblyPiece &p : mine)
        {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x + sizes[p.frame][0]);
            maxY = std::max(maxY, p.y + sizes[p.frame][1]);
        }
        Block b;
        for (AssemblyPiece p : mine)
        {
            p.x -= minX;
            p.y -= minY;
            b.pieces.push_back(p);
        }
        b.w = std::max(1, maxX - minX);
        b.h = std::max(1, maxY - minY);
        blocks.push_back(std::move(b));
    }
    for (std::size_t i = 0; i < sizes.size(); ++i)
        if (!placed[i])
        {
            Block b;
            b.pieces.push_back({static_cast<int>(i), 0, 0});
            b.w = std::max(1, sizes[i][0]);
            b.h = std::max(1, sizes[i][1]);
            blocks.push_back(std::move(b));
        }
    std::vector<std::array<int, 2>> blockSizes;
    for (const Block &b : blocks)
        blockSizes.push_back({b.w, b.h});
    const auto at = ShelfPack(blockSizes, padding);
    std::vector<std::array<int, 2>> out(sizes.size(), {0, 0});
    for (std::size_t i = 0; i < blocks.size(); ++i)
        for (const AssemblyPiece &p : blocks[i].pieces)
            out[p.frame] = {at[i][0] + p.x, at[i][1] + p.y};
    return out;
}

ScaledGroupLayout ScaleGroupRects(
    const std::vector<std::array<int, 4>> &selected,
    const std::vector<std::array<int, 4>> &unselected,
    float scaleX,
    float scaleY,
    int anchorX,
    int anchorY,
    int canvasW,
    int canvasH,
    int padding)
{
    ScaledGroupLayout res;
    res.canvasW = std::max(1, canvasW);
    res.canvasH = std::max(1, canvasH);
    if (selected.empty())
        return res;

    auto rectsOverlap = [](const std::array<int, 4> &a, const std::array<int, 4> &b) {
        return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
    };

    const float sx = std::max(0.01f, scaleX);
    const float sy = std::max(0.01f, scaleY);
    res.rects.reserve(selected.size());
    int minX = std::numeric_limits<int>::max(), minY = std::numeric_limits<int>::max();
    for (const auto &r : selected)
    {
        const int nx = anchorX + static_cast<int>(std::lround((r[0] - anchorX) * static_cast<double>(sx)));
        const int ny = anchorY + static_cast<int>(std::lround((r[1] - anchorY) * static_cast<double>(sy)));
        const int nw = std::max(1, static_cast<int>(std::lround(r[2] * static_cast<double>(sx))));
        const int nh = std::max(1, static_cast<int>(std::lround(r[3] * static_cast<double>(sy))));
        res.rects.push_back({nx, ny, nw, nh});
        minX = std::min(minX, nx);
        minY = std::min(minY, ny);
    }
    const int shiftX = minX < 0 ? -minX : 0;
    const int shiftY = minY < 0 ? -minY : 0;
    if (shiftX || shiftY)
        for (auto &r : res.rects)
        {
            r[0] += shiftX;
            r[1] += shiftY;
        }

    bool conflict = false;
    for (std::size_t i = 0; i < res.rects.size() && !conflict; ++i)
    {
        for (const auto &u : unselected)
            if (rectsOverlap(res.rects[i], u))
            {
                conflict = true;
                break;
            }
        for (std::size_t j = i + 1; j < res.rects.size() && !conflict; ++j)
            if (!rectsOverlap(selected[i], selected[j]) && rectsOverlap(res.rects[i], res.rects[j]))
                conflict = true;
    }

    if (conflict)
    {
        std::vector<std::array<int, 2>> sizes;
        sizes.reserve(res.rects.size());
        for (const auto &r : res.rects)
            sizes.push_back({r[2], r[3]});
        const int pad = std::max(0, padding);
        const auto pos = ShelfPack(sizes, pad);
        int baseY = 0;
        for (const auto &u : unselected)
            baseY = std::max(baseY, u[1] + u[3] + pad);
        for (std::size_t i = 0; i < res.rects.size(); ++i)
        {
            res.rects[i][0] = pos[i][0];
            res.rects[i][1] = baseY + pos[i][1];
        }
        res.relocated = true;
    }

    for (const auto &r : res.rects)
    {
        res.canvasW = std::max(res.canvasW, r[0] + r[2]);
        res.canvasH = std::max(res.canvasH, r[1] + r[3]);
    }
    return res;
}

} // namespace CharEditor::SpriteMerge
