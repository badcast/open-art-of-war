#include "core/JsonFile.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <yaml-cpp/yaml.h>

namespace OpenArtOfWar::JsonFile
{

namespace fs = std::filesystem;

namespace
{

nlohmann::json YamlToJson(const YAML::Node &node)
{
    if (!node.IsDefined() || node.IsNull())
        return nullptr;
    if (node.IsScalar())
    {
        const std::string &scalar = node.Scalar();
        if (scalar == "true" || scalar == "True" || scalar == "TRUE")
            return true;
        if (scalar == "false" || scalar == "False" || scalar == "FALSE")
            return false;
        int64_t i = 0;
        if (YAML::convert<int64_t>::decode(node, i))
            return i;
        double d = 0;
        if (YAML::convert<double>::decode(node, d))
            return d;
        return scalar;
    }
    if (node.IsSequence())
    {
        nlohmann::json j = nlohmann::json::array();
        for (const auto &item : node)
            j.push_back(YamlToJson(item));
        return j;
    }
    if (node.IsMap())
    {
        nlohmann::json j = nlohmann::json::object();
        for (const auto &it : node)
            j[it.first.as<std::string>()] = YamlToJson(it.second);
        return j;
    }
    return nullptr;
}

} // namespace

nlohmann::json ReadFromString(std::string_view text)
{
    nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false, true);
    if (j.is_object())
        return j;

    try
    {
        YAML::Node root = YAML::Load(std::string(text));
        if (root.IsMap())
        {
            nlohmann::json yj = YamlToJson(root);
            if (yj.is_object())
                return yj;
        }
    }
    catch (...)
    {}

    return nlohmann::json::object();
}

nlohmann::json ReadYamlFromString(std::string_view text)
{
    try
    {
        YAML::Node root = YAML::Load(std::string(text));
        if (root.IsMap())
        {
            nlohmann::json yj = YamlToJson(root);
            if (yj.is_object())
                return yj;
        }
    }
    catch (...)
    {}
    return nlohmann::json::object();
}

nlohmann::json ReadObjectFromFileName(std::string_view path)
{
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in)
        return nlohmann::json::object();
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ReadFromString(text);
}

bool WriteAtomic(std::string_view path, const nlohmann::json &doc)
{
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << doc.dump(2) << '\n';
        out.flush();
        if (!out.good())
            return false;
    }
    fs::rename(tmp, target, ec); // replaces atomically on POSIX and NTFS
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace OpenArtOfWar::JsonFile
