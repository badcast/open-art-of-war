#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace OpenArtOfWar::JsonFile
{
    // The Json ojject in `text`; an empty object when the text is not an object
    nlohmann::json readFromString(const std::string &text);

    // The JSON object in `path`; an empty object when the file is missing or not an object
    nlohmann::json readObjectFromFileName(const std::string &path);

    // Writes `doc` to `path` through a temporary file and a rename, so a crash never leaves half a file
    bool writeAtomic(const std::string &path, const nlohmann::json &doc);

} // namespace OpenArtOfWar::JsonFile
