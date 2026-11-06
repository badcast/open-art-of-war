#pragma once

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <string>

namespace CharEditor
{

// Saving a config without losing what the editor does not know about.
//
// The editor keeps the document as it was loaded (`base`) next to the edited one (`mine`). On saving, the file
// is read again (`disk`) and only the editor's own changes (mine vs base) are applied to it: keys the editor
// never touched — fields of a newer game version, keys added by hand or by another tool after the editor
// loaded the file — stay exactly as they are on disk.
//
// Maps are merged key by key; a changed scalar or list replaces the one on disk; a key the editor removed is
// removed. A key whose type differs between mine and disk takes the editor's value only where the editor
// changed it.
YAML::Node MergeYamlEdits(const YAML::Node &base, const YAML::Node &mine, const YAML::Node &disk);
nlohmann::ordered_json MergeJsonEdits(
    const nlohmann::ordered_json &base, const nlohmann::ordered_json &mine, const nlohmann::ordered_json &disk);

// The file as YAML; an undefined node when it is missing or does not parse
YAML::Node ReadYamlFile(const std::filesystem::path &path);

// Writes through a temporary file next to `path` and a rename: a crash never leaves half a config
bool WriteFileAtomic(const std::filesystem::path &path, const std::string &text, std::string &error);

// YAML text of a node the way the editor writes it (two spaces of indentation, trailing newline)
std::string YamlText(const YAML::Node &node);

} // namespace CharEditor
