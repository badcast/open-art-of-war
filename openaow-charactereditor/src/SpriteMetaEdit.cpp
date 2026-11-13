#include "SpriteMetaEdit.hpp"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <regex>
#include <sstream>

namespace CharEditor
{

namespace
{

std::string FormatCoord(float v)
{
    const float snapped = std::round(v * 20.0f) / 20.0f;
    if (std::fabs(snapped) < 1e-4f)
        return "0";
    if (std::fabs(snapped - std::round(snapped)) < 1e-4f)
        return std::to_string(static_cast<long long>(std::lround(snapped)));
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), snapped, std::chars_format::fixed, 2);
    std::string s(buf, r.ptr);
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

YAML::Node ResolveItemSeqNode(const YAML::Node &root, std::string_view section, std::string_view itemName)
{
    if (!root || !root.IsMap())
        return {};
    const YAML::Node sec = root[std::string(section)];
    if (!sec || !sec.IsMap())
        return {};
    const YAML::Node item = sec[std::string(itemName)];
    if (!item)
        return {};
    if (section == "buildings")
        return (item.IsMap() && item["parts"] && item["parts"].IsSequence()) ? item["parts"] : YAML::Node();
    return item.IsSequence() ? item : YAML::Node();
}

int LeadingSpaces(std::string_view line)
{
    int n = 0;
    while (n < static_cast<int>(line.size()) && line[n] == ' ')
        ++n;
    return n;
}

std::string TrimView(std::string_view s)
{
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return std::string(s.substr(a, b - a));
}

// Extracts scalar map key if trimmed line looks like `key:` or `key: ...` (and not a sequence `- ` item).
bool ParseMappingKey(std::string_view trimmed, std::string &outKey)
{
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '-')
        return false;
    const auto colon = trimmed.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    std::string_view candidate = trimmed.substr(0, colon);
    for (char c : candidate)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    outKey.assign(candidate);
    return true;
}

std::string PatchAtOnLine(const std::string &line, float x, float y, bool &patched)
{
    patched = false;
    const bool hasCr = !line.empty() && line.back() == '\r';
    std::string clean = hasCr ? line.substr(0, line.size() - 1) : line;

    // Preserve any trailing comment outside quotes/brackets
    std::size_t commentPos = std::string::npos;
    int braceDepth = 0;
    bool inSingle = false, inDouble = false;
    for (std::size_t i = 0; i < clean.size(); ++i)
    {
        const char ch = clean[i];
        if (ch == '\'' && !inDouble)
            inSingle = !inSingle;
        else if (ch == '"' && !inSingle)
            inDouble = !inDouble;
        else if (!inSingle && !inDouble)
        {
            if (ch == '{' || ch == '[')
                ++braceDepth;
            else if (ch == '}' || ch == ']')
                braceDepth = std::max(0, braceDepth - 1);
            else if (ch == '#' && braceDepth == 0)
            {
                commentPos = i;
                break;
            }
        }
    }
    const std::string tail = commentPos != std::string::npos ? clean.substr(commentPos) : std::string();
    std::string body = commentPos != std::string::npos ? clean.substr(0, commentPos) : clean;

    const std::string replacement = "at: [" + FormatCoord(x) + ", " + FormatCoord(y) + "]";
    static const std::regex kAtRegex(R"(\bat\s*:\s*\[[^\]]*\])");
    if (std::regex_search(body, kAtRegex))
    {
        body = std::regex_replace(body, kAtRegex, replacement, std::regex_constants::format_first_only);
        patched = true;
    }
    else
    {
        const auto closeBrace = body.rfind('}');
        if (closeBrace != std::string::npos)
        {
            body.insert(closeBrace, ", " + replacement);
            patched = true;
        }
    }
    if (!patched)
        return line;
    return body + tail + (hasCr ? "\r" : "");
}

std::string RotSlotSuffix(std::string_view target)
{
    if (target.rfind("rot_", 0) != 0)
        return {};
    const auto open = target.find('[');
    const auto close = target.find(']', open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
        return {};
    return std::string(target.substr(open, close - open + 1));
}

} // namespace

std::vector<MetaLayerRef> ReadMetaItemLayers(
    const std::string &metaText, std::string_view section, std::string_view itemName)
{
    std::vector<MetaLayerRef> out;
    if (metaText.empty() || section.empty() || itemName.empty())
        return out;
    try
    {
        const YAML::Node root = YAML::Load(metaText);
        const YAML::Node seq = ResolveItemSeqNode(root, section, itemName);
        if (!seq || !seq.IsSequence())
            return out;
        for (int i = 0; i < static_cast<int>(seq.size()); ++i)
        {
            const YAML::Node e = seq[i];
            if (!e || !e.IsMap())
                continue;
            MetaLayerRef ref;
            ref.index = i;
            for (const char *k : {"frame", "part", "anim", "seq"})
            {
                if (e[k] && e[k].IsScalar())
                {
                    ref.kind = k;
                    ref.target = e[k].as<std::string>("");
                    break;
                }
            }
            if (e["at"] && e["at"].IsSequence() && e["at"].size() >= 2)
            {
                ref.x = e["at"][0].as<float>(0.0f);
                ref.y = e["at"][1].as<float>(0.0f);
                ref.hasAt = true;
            }
            if (e["flag"] && e["flag"].IsScalar())
                ref.flag = e["flag"].as<int>(0);
            if (e["state"] && e["state"].IsScalar())
                ref.state = e["state"].as<int>(0);
            out.push_back(std::move(ref));
        }
    }
    catch (const std::exception &)
    {}
    return out;
}

std::vector<std::string> ReadMetaFamilyMembers(
    const std::string &metaText, std::string_view familyName, std::string_view memberKey)
{
    std::vector<std::string> out;
    if (metaText.empty() || familyName.empty() || memberKey.empty())
        return out;
    try
    {
        const YAML::Node root = YAML::Load(metaText);
        const YAML::Node fams = root["families"];
        if (!fams || !fams.IsMap())
            return out;
        const YAML::Node fam = fams[std::string(familyName)];
        if (!fam || !fam.IsMap())
            return out;
        const YAML::Node seq = fam[std::string(memberKey)];
        if (!seq || !seq.IsSequence())
            return out;
        for (const auto &el : seq)
            if (el.IsScalar())
                out.push_back(el.as<std::string>(""));
    }
    catch (const std::exception &)
    {}
    return out;
}

bool SetMetaLayerAt(
    std::string &metaText,
    std::string_view section,
    std::string_view itemName,
    int layerIndex,
    float x,
    float y)
{
    if (metaText.empty() || layerIndex < 0)
        return false;

    const bool hasTrailingNewline = !metaText.empty() && metaText.back() == '\n';
    std::vector<std::string> lines;
    {
        std::istringstream in(metaText);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(std::move(line));
    }

    const bool isBuilding = (section == "buildings");
    bool inSection = false;
    bool inItem = false;
    bool inBuildingParts = false;
    int itemIndent = -1;
    int bPartsIndent = -1;
    int curEntry = -1;
    bool changed = false;

    for (std::string &line : lines)
    {
        const std::string trimmed = TrimView(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        const int indent = LeadingSpaces(line);
        if (indent == 0)
        {
            std::string topKey;
            inSection = ParseMappingKey(trimmed, topKey) && (topKey == section);
            inItem = false;
            inBuildingParts = false;
            itemIndent = -1;
            bPartsIndent = -1;
            curEntry = -1;
            continue;
        }
        if (!inSection)
            continue;

        std::string key;
        if (ParseMappingKey(trimmed, key))
        {
            if (!inItem || indent <= itemIndent)
            {
                inItem = (key == itemName);
                itemIndent = inItem ? indent : -1;
                inBuildingParts = false;
                bPartsIndent = -1;
                curEntry = -1;
            }
            else if (isBuilding && inItem)
            {
                if (!inBuildingParts || indent <= bPartsIndent)
                {
                    inBuildingParts = (key == "parts");
                    bPartsIndent = inBuildingParts ? indent : -1;
                    curEntry = -1;
                }
            }
            continue;
        }

        const bool inTargetSeq = isBuilding ? (inItem && inBuildingParts) : inItem;
        if (inTargetSeq && (trimmed.rfind("- ", 0) == 0 || trimmed == "-"))
        {
            ++curEntry;
            if (curEntry == layerIndex)
            {
                line = PatchAtOnLine(line, x, y, changed);
                break;
            }
        }
    }

    if (!changed)
        return false;

    std::string rebuilt;
    rebuilt.reserve(metaText.size() + 32);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        rebuilt += lines[i];
        if (i + 1 < lines.size() || hasTrailingNewline)
            rebuilt += '\n';
    }
    metaText = std::move(rebuilt);
    return true;
}

bool ShiftPartPivot(
    std::string &metaText,
    std::string_view partName,
    float pivotX,
    float pivotY,
    bool syncMatchingParts)
{
    const std::vector<MetaLayerRef> baseLayers = ReadMetaItemLayers(metaText, "parts", partName);
    if (baseLayers.empty())
        return false;
    if (std::fabs(pivotX) < 1e-4f && std::fabs(pivotY) < 1e-4f)
        return true;

    const float dx = -pivotX;
    const float dy = -pivotY;
    const MetaLayerRef &anchor = baseLayers.front();

    bool anyChanged = false;
    for (const MetaLayerRef &l : baseLayers)
        anyChanged |= SetMetaLayerAt(metaText, "parts", partName, l.index, l.x + dx, l.y + dy);

    if (syncMatchingParts && !anchor.kind.empty() && !anchor.target.empty())
    {
        try
        {
            const YAML::Node root = YAML::Load(metaText);
            const YAML::Node parts = root["parts"];
            if (parts && parts.IsMap())
            {
                for (const auto &kv : parts)
                {
                    if (!kv.first.IsScalar())
                        continue;
                    const std::string otherName = kv.first.as<std::string>("");
                    if (otherName.empty() || otherName == partName)
                        continue;
                    const std::vector<MetaLayerRef> otherLayers = ReadMetaItemLayers(metaText, "parts", otherName);
                    bool sharesAnchor = false;
                    for (const MetaLayerRef &ol : otherLayers)
                    {
                        if (ol.kind == anchor.kind && ol.target == anchor.target
                            && std::fabs(ol.x - anchor.x) < 0.05f && std::fabs(ol.y - anchor.y) < 0.05f)
                        {
                            sharesAnchor = true;
                            break;
                        }
                    }
                    if (sharesAnchor)
                    {
                        for (const MetaLayerRef &ol : otherLayers)
                            anyChanged |= SetMetaLayerAt(metaText, "parts", otherName, ol.index, ol.x + dx, ol.y + dy);
                    }
                }
            }
        }
        catch (const std::exception &)
        {}
    }
    return anyChanged;
}

bool ShiftBodyLayer(
    std::string &metaText,
    std::string_view bodyName,
    int layerIndex,
    float dx,
    float dy,
    bool syncSiblingRot)
{
    const std::vector<MetaLayerRef> layers = ReadMetaItemLayers(metaText, "bodies", bodyName);
    if (layerIndex < 0 || layerIndex >= static_cast<int>(layers.size()))
        return false;
    const MetaLayerRef target = layers[layerIndex];
    const std::string slot = RotSlotSuffix(target.target);
    bool anyChanged = SetMetaLayerAt(metaText, "bodies", bodyName, layerIndex, target.x + dx, target.y + dy);
    if (syncSiblingRot && !slot.empty())
    {
        for (const MetaLayerRef &other : layers)
        {
            if (other.index == layerIndex)
                continue;
            if (RotSlotSuffix(other.target) == slot)
                anyChanged |= SetMetaLayerAt(metaText, "bodies", bodyName, other.index, other.x + dx, other.y + dy);
        }
    }
    return anyChanged;
}

} // namespace CharEditor
