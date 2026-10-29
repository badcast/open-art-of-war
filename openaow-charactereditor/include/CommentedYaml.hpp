#pragma once

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace CharEditor {

// A YAML file edited without losing its comments: values are replaced in the text, the document is parsed
// again from it. Keys are addressed by a dotted path ("easy.tactics.leash_tiles"); only "key: value" lines of
// maps are supported (no lists of maps), which is all the settings files use.
class CommentedYaml {
public:
    bool load(const std::filesystem::path& path, std::string& error);
    bool save(std::string& error);

    bool loaded() const { return m_loaded; }
    bool dirty() const { return m_dirty; }
    const std::filesystem::path& path() const { return m_path; }
    const YAML::Node& root() const { return m_node; }
    std::string text() const;

    // Node at the path; an undefined node when there is none
    YAML::Node at(const std::string& path) const;
    // The comment lines right above the key
    std::string comment(const std::string& path) const;
    // Replaces the value of a "key: value" line; valueText is YAML ("12", "true", "\"name\"", "[1, 2]").
    // False (nothing changed) when the key has no value line or the result would not parse.
    bool set(const std::string& path, const std::string& valueText);

private:
    void index();

    std::filesystem::path m_path;
    std::vector<std::string> m_lines;
    std::map<std::string, int> m_lineOf;
    std::map<std::string, std::string> m_comments;
    YAML::Node m_node;
    bool m_loaded{false};
    bool m_dirty{false};
};

} // namespace CharEditor
