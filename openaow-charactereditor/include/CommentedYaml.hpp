#pragma once

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CharEditor
{

// A YAML file edited without losing its comments: values are replaced in the text, the document is parsed
// again from it. Keys are addressed by a dotted path ("easy.tactics.leash_tiles"); only "key: value" lines of
// maps are supported (no lists of maps), which is all the settings files use.
class CommentedYaml
{
public:
    bool load(const std::filesystem::path &path, std::string &error);
    bool Save(std::string &error);

    bool Loaded() const
    {
        return m_loaded;
    }
    bool Dirty() const
    {
        return m_dirty;
    }
    const std::filesystem::path &Path() const
    {
        return m_path;
    }
    const YAML::Node &Root() const
    {
        return m_node;
    }
    std::string text() const;

    // Node at the path; an undefined node when there is none
    YAML::Node At(std::string_view path) const;
    // The comment lines right above the key
    std::string Comment(std::string_view path) const;
    // Replaces the value of a "key: value" line; valueText is YAML ("12", "true", "\"name\"", "[1, 2]").
    // False (nothing changed) when the key has no value line or the result would not parse.
    bool Set(std::string_view path, std::string_view valueText);

private:
    void Index();

    std::filesystem::path m_path;
    // Values set since the last load/save, in order: on saving they are applied again to the file as it is
    // then, so lines and keys added there meanwhile (by hand, a newer game) are not lost
    std::vector<std::pair<std::string, std::string>> m_edits;
    std::vector<std::string> m_lines;
    std::map<std::string, int, std::less<>> m_lineOf;
    std::map<std::string, std::string, std::less<>> m_comments;
    YAML::Node m_node;
    bool m_loaded {false};
    bool m_dirty {false};
};

} // namespace CharEditor
