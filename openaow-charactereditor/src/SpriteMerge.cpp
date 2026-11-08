#include "SpriteMerge.hpp"

#include <algorithm>
#include <random>
#include <cctype>
#include <cmath>
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

} // namespace CharEditor::SpriteMerge
