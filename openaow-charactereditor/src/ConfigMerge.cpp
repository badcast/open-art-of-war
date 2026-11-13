#include "ConfigMerge.hpp"

#include <fstream>
#include <system_error>

namespace CharEditor
{

namespace
{

bool SameYaml(const YAML::Node &a, const YAML::Node &b)
{
    if (!a.IsDefined() || !b.IsDefined())
        return a.IsDefined() == b.IsDefined();
    if (a.Type() != b.Type())
        return false;
    return YAML::Dump(a) == YAML::Dump(b);
}

} // namespace

YAML::Node MergeYamlEdits(const YAML::Node &base, const YAML::Node &mine, const YAML::Node &disk)
{
    // Nothing on disk to keep (or not a map): the editor's document as it is
    if (!disk.IsDefined() || disk.IsNull())
        return YAML::Clone(mine);
    // Untouched by the editor: the disk version wins (whatever it is now)
    if (SameYaml(base, mine))
        return YAML::Clone(disk);
    // Maps on all sides: key by key
    if (mine.IsMap() && disk.IsMap() && (!base.IsDefined() || base.IsMap()))
    {
        YAML::Node out = YAML::Clone(disk);
        for (const auto &kv : mine)
        {
            const std::string key = kv.first.as<std::string>();
            const YAML::Node b = base.IsDefined() && base.IsMap() ? base[key] : YAML::Node();
            const YAML::Node d = disk[key];
            if (SameYaml(b, kv.second))
                continue; // not changed by the editor
            out[key] = d.IsDefined() ? MergeYamlEdits(b, kv.second, d) : YAML::Clone(kv.second);
        }
        // Keys the editor removed
        if (base.IsDefined() && base.IsMap())
        {
            for (const auto &kv : base)
            {
                const std::string key = kv.first.as<std::string>();
                if (!mine[key].IsDefined() && out[key].IsDefined() && SameYaml(kv.second, disk[key]))
                    out.remove(key);
            }
        }
        return out;
    }
    // A changed scalar or list: the editor's value
    return YAML::Clone(mine);
}

nlohmann::ordered_json MergeJsonEdits(
    const nlohmann::ordered_json &base, const nlohmann::ordered_json &mine, const nlohmann::ordered_json &disk)
{
    if (disk.is_discarded() || disk.is_null())
        return mine;
    if (base == mine)
        return disk;
    if (mine.is_object() && disk.is_object() && (base.is_null() || base.is_object()))
    {
        nlohmann::ordered_json out = disk;
        for (auto it = mine.begin(); it != mine.end(); ++it)
        {
            const nlohmann::ordered_json b
                = base.is_object() && base.contains(it.key()) ? base[it.key()] : nlohmann::ordered_json();
            if (b == it.value())
                continue;
            out[it.key()] = disk.contains(it.key()) ? MergeJsonEdits(b, it.value(), disk[it.key()]) : it.value();
        }
        if (base.is_object())
        {
            for (auto it = base.begin(); it != base.end(); ++it)
            {
                if (!mine.contains(it.key()) && disk.contains(it.key()) && disk[it.key()] == it.value())
                    out.erase(it.key());
            }
        }
        return out;
    }
    return mine;
}

YAML::Node ReadYamlFile(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return YAML::Node(YAML::NodeType::Undefined);
    try
    {
        return YAML::LoadFile(path.string());
    }
    catch (const std::exception &)
    {
        return YAML::Node(YAML::NodeType::Undefined);
    }
}

bool WriteFileAtomic(const std::filesystem::path &path, std::string_view text, std::string &error)
{
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "Could not open file for writing: " + tmp.string();
            return false;
        }
        out << text;
        out.flush();
        if (!out)
        {
            error = "Failed to write file: " + tmp.string();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        error = "Could not replace " + path.string() + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string YamlText(const YAML::Node &node)
{
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << node;
    return std::string(emitter.c_str()) + "\n";
}

} // namespace CharEditor
