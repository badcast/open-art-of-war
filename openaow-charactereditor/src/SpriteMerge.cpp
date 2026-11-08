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

std::string Str(const YAML::Node &n, const char *key, const std::string &def)
{
    try
    {
        if (n && n.IsMap() && n[key] && n[key].IsScalar())
            return n[key].as<std::string>();
    }
    catch (...)
    {}
    return def;
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
void RewriteEntries(YAML::Node n, const std::string &from, const NameMap &names)
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
void RewriteFamily(YAML::Node fam, const std::string &from, const NameMap &names)
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

std::string RewriteMetaRef(const std::string &ref, const std::string &fromObject, const NameMap &names)
{
    const auto dot = ref.find('.');
    const std::string object = dot == std::string::npos ? fromObject : ref.substr(0, dot);
    std::string name = dot == std::string::npos ? ref : ref.substr(dot + 1);
    const auto it = names.find(object);
    if (it == names.end())
        return ref; // another object: unchanged
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

YAML::Node MergeMetas(
    const std::vector<Source> &sources,
    const NameMap &names,
    const FramePlaces &places,
    const std::string &image,
    const std::string &resistanceImage,
    int imageW,
    int imageH)
{
    YAML::Node out(YAML::NodeType::Map);
    if (sources.empty())
        return out;
    const YAML::Node &first = sources.front().meta;
    const float scale = Scale(first);
    const std::string recolor = Str(first, "recolor", "none");
    out["image"] = image;
    out["image_size"] = FlowSeq({double(imageW), double(imageH)});
    if (!resistanceImage.empty())
        out["resistance_image"] = resistanceImage;
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
                    const YAML::Node rect = value.IsMap() ? value["rect"] : value;
                    double w = 1, h = 1;
                    if (rect && rect.IsSequence() && rect.size() >= 4)
                    {
                        w = rect[2].as<double>(1.0);
                        h = rect[3].as<double>(1.0);
                    }
                    YAML::Node f(YAML::NodeType::Map);
                    f.SetStyle(YAML::EmitterStyle::Flow);
                    f["rect"] = FlowSeq({double(place->second[0]), double(place->second[1]), w, h});
                    if (value.IsMap() && value["size"])
                        f["size"] = value["size"];
                    else if (std::fabs(srcScale - scale) > 1e-4f)
                        f["size"] = FlowSeq({w / srcScale, h / srcScale});
                    const std::string fr = value.IsMap() ? Str(value, "recolor", srcRecolor) : srcRecolor;
                    if (fr != recolor)
                        f["recolor"] = fr;
                    if (value.IsMap())
                        for (const auto &e : value)
                        {
                            const std::string k = e.first.as<std::string>("");
                            if (k != "rect" && k != "size" && k != "recolor")
                                f[k] = e.second;
                        }
                    value = f;
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

std::string EmitMeta(const YAML::Node &meta, const std::string &header)
{
    YAML::Emitter e;
    e.SetIndent(2);
    e.SetSeqFormat(YAML::Block);
    e << meta;
    std::string text = header + e.c_str();
    if (text.empty() || text.back() != '\n')
        text += '\n';
    return text;
}

std::string RewriteText(const std::string &text, const NameMap &names, const std::string &merged, int &count)
{
    count = 0;
    std::string out;
    out.reserve(text.size());
    std::istringstream in(text);
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
                if (names.count(value))
                {
                    line = line.substr(0, v) + quote + merged + quote + line.substr(end);
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
                    res += merged + "." + item->second;
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

bool BindsBare(const std::string &text, const std::string &object)
{
    std::istringstream in(text);
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

std::vector<AssemblyGroup> AssemblyGroupsFromMeta(
    const YAML::Node &meta, const std::string &object, const std::map<std::string, int> &frameIndex, float scale)
{
    std::vector<AssemblyGroup> groups;
    const YAML::Node frames = meta["frames"], parts = meta["parts"], anims = meta["anims"], families = meta["families"];
    // "object.item[i]" / "item[i]" -> {item, i} for this object; another object's name is not ours (empty)
    auto local = [&](std::string name, int &index) -> std::string {
        index = 0;
        const std::string prefix = object + ".";
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
    auto member = [&](const std::string &family, const char *kind, int index) -> std::string {
        if (!families || !families.IsMap())
            return {};
        const YAML::Node f = families[family];
        if (!f || !f.IsMap() || !f[kind] || !f[kind].IsSequence() || index < 0
            || index >= static_cast<int>(f[kind].size()))
            return {};
        return f[kind][index].as<std::string>();
    };

    AssemblyGroup group;
    std::function<void(const YAML::Node &, int, int, int)> walk;
    // One entry of a composite: a frame where it is drawn, a part / the first picture of an animation opened up
    auto entry = [&](const YAML::Node &e, int ox, int oy, int depth) {
        if (!e.IsMap())
            return;
        int ax = ox, ay = oy;
        if (const YAML::Node at = e["at"]; at && at.IsSequence() && at.size() == 2)
        {
            ax += static_cast<int>(std::lround(at[0].as<float>() * scale));
            ay += static_cast<int>(std::lround(at[1].as<float>() * scale));
        }
        int index = 0;
        if (const YAML::Node f = e["frame"])
        {
            std::string name = local(f.as<std::string>(), index);
            if (!name.empty() && !(frames && frames[name]))
                name = member(name, "frames", index);
            const auto it = frameIndex.find(name);
            if (it != frameIndex.end())
                group.push_back({it->second, ax, ay});
        }
        else if (const YAML::Node p = e["part"]; p && depth < 6)
        {
            std::string name = local(p.as<std::string>(), index);
            if (!name.empty() && !(parts && parts[name]))
                name = member(name, "parts", index);
            if (!name.empty() && parts && parts[name])
                walk(parts[name], ax, ay, depth + 1);
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
                walk(first, ax, ay, depth + 1);
            }
        }
    };
    walk = [&](const YAML::Node &list, int ox, int oy, int depth) {
        if (!list.IsSequence())
            return;
        for (const auto &e : list)
        {
            try
            {
                entry(e, ox, oy, depth);
            }
            catch (const std::exception &)
            {}
        }
    };
    auto close = [&]() {
        if (!group.empty())
            groups.push_back(std::move(group));
        group.clear();
    };
    // What the game draws as a whole first: a unit body (hull, turret, shadow...), a building with all its parts
    if (const YAML::Node bodies = meta["bodies"]; bodies && bodies.IsMap())
        for (const auto &kv : bodies)
        {
            walk(kv.second, 0, 0, 0);
            close();
        }
    if (const YAML::Node buildings = meta["buildings"]; buildings && buildings.IsMap())
        for (const auto &kv : buildings)
        {
            if (kv.second.IsMap())
                walk(kv.second["parts"], 0, 0, 0);
            close();
        }
    // Then the composites on their own (frames no body / building shows, e.g. the later pictures of an animation)
    if (parts && parts.IsMap())
        for (const auto &kv : parts)
        {
            walk(kv.second, 0, 0, 0);
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
                // A layer drawn over another one (a turret over its hull): the nearest free place around where the
                // game draws it, ring by ring
                bool found = false;
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

} // namespace CharEditor::SpriteMerge
