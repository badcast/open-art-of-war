#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace OpenArtOfWar
{

    // System environment of the game: assets/data/game_environment.json holds the factory defaults (fog look,
    // construction rules, network ports, render preferences) and is read-only, as it ships inside data.blob.
    // What the game finds out at runtime (the "render" section: the API that works here) is kept in
    // settings.json of the per-user directory, key "environment", and laid over the defaults on load.
    class EnvironmentConfig
    {
    public:
        static constexpr const char *kAssetPath = "assets/data/game_environment.json";

        bool load(const std::string &assetPath = kAssetPath);
        // Writes the runtime sections into settings.json if anything changed (atomically: temp file + rename)
        bool save();

        const nlohmann::json &root() const
        {
            return m_root;
        }
        // Section `name`; an empty object when it is missing
        const nlohmann::json &section(const std::string &name) const;
        // Writable section, created on demand; marks the config as changed
        nlohmann::json &edit(const std::string &name);

        bool isDirty() const
        {
            return m_dirty;
        }

    private:
        nlohmann::json m_root = nlohmann::json::object();
        std::string m_assetPath {kAssetPath};
        bool m_dirty {false};
    };

} // namespace OpenArtOfWar
