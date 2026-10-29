#include "core/EnvironmentConfig.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include "core/Settings.hpp"
#include <filesystem>
#include <iostream>

namespace OpenArtOfWar
{

    namespace fs = std::filesystem;

    namespace
    {

        // The sections the game itself writes; everything else comes from the factory defaults
        const char *const kRuntimeSections[] = {"render"};
        // Key of the runtime sections inside settings.json
        const char *const kSettingsKey = "environment";

    } // namespace

    bool EnvironmentConfig::load(const std::string &assetPath)
    {
        m_assetPath = assetPath;
        m_dirty = false;
        std::string text;
        const bool found = Assets::readText(assetPath, text);
        m_root = nlohmann::json::parse(text, nullptr, false, true);
        if(!found || !m_root.is_object())
            m_root = nlohmann::json::object();

        // What the game found out on this system (settings.json) wins over the factory defaults
        nlohmann::json runtime = JsonFile::readObject(Settings::defaultPath()).value(kSettingsKey, nlohmann::json::object());
        // Earlier builds kept it in a file of its own: take it over once
        const fs::path legacy = fs::path(Assets::userDataDir()) / "game_environment.json";
        std::error_code ec;
        if(fs::exists(legacy, ec))
        {
            if(!runtime.is_object() || runtime.empty())
            {
                runtime = JsonFile::readObject(legacy.string());
                m_dirty = true;
            }
            fs::remove(legacy, ec);
        }
        if(runtime.is_object())
        {
            for(const char *name : kRuntimeSections)
            {
                if(runtime.contains(name) && runtime[name].is_object())
                    m_root[name] = runtime[name];
            }
        }
        if(m_dirty)
            save();
        return found;
    }

    const nlohmann::json &EnvironmentConfig::section(const std::string &name) const
    {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        auto it = m_root.find(name);
        return (it != m_root.end() && it->is_object()) ? *it : kEmpty;
    }

    nlohmann::json &EnvironmentConfig::edit(const std::string &name)
    {
        m_dirty = true;
        nlohmann::json &s = m_root[name];
        if(!s.is_object())
            s = nlohmann::json::object();
        return s;
    }

    bool EnvironmentConfig::save()
    {
        if(!m_dirty)
            return true;
        // The asset file holds the factory defaults (and ends up in data.blob): only the runtime sections
        // are written, into settings.json next to the player's options (which are left as they are)
        const std::string path = Settings::defaultPath();
        nlohmann::json settings = JsonFile::readObject(path);
        nlohmann::json runtime = nlohmann::json::object();
        for(const char *name : kRuntimeSections)
        {
            if(m_root.contains(name))
                runtime[name] = m_root[name];
        }
        settings[kSettingsKey] = runtime;
        if(!JsonFile::writeAtomic(path, settings))
        {
            std::cerr << "[Environment] Could not write " << path << std::endl;
            return false;
        }
        m_dirty = false;
        return true;
    }

} // namespace OpenArtOfWar
