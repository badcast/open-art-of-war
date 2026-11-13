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

bool EnvironmentConfig::Load(std::string_view assetPath)
{
    m_assetPath.assign(assetPath);
    m_dirty = false;
    std::string text;
    const bool found = Assets::ReadText(assetPath, text);
    m_root = JsonFile::ReadFromString(text);
    if (!found || !m_root.is_object())
        m_root = nlohmann::json::object();

    // What the game found out on this system (settings.json) wins over the factory defaults
    nlohmann::json runtime
        = JsonFile::ReadObjectFromFileName(Settings::DefaultPath()).value(kSettingsKey, nlohmann::json::object());
    // Earlier builds kept it in a file of its own: take it over once
    const fs::path legacy = fs::path(Assets::UserDataDir()) / "game_environment.json";
    std::error_code ec;
    if (fs::exists(legacy, ec))
    {
        if (!runtime.is_object() || runtime.empty())
        {
            runtime = JsonFile::ReadObjectFromFileName(legacy.string());
            m_dirty = true;
        }
        fs::remove(legacy, ec);
    }
    if (runtime.is_object())
    {
        for (const char *name : kRuntimeSections)
        {
            if (runtime.contains(name) && runtime[name].is_object())
                m_root[name] = runtime[name];
        }
    }
    if (m_dirty)
        Save();
    return found;
}

nlohmann::json EnvironmentConfig::FactorySettings(std::string_view assetPath)
{
    std::string text;
    if (!Assets::ReadText(assetPath, text))
        return nlohmann::json::object();
    const nlohmann::json root = JsonFile::ReadFromString(text);
    if (root.is_object() && root.contains("settings") && root["settings"].is_object())
        return root["settings"];
    return nlohmann::json::object();
}

const nlohmann::json &EnvironmentConfig::Section(std::string_view name) const
{
    static const nlohmann::json kEmpty = nlohmann::json::object();
    auto it = m_root.find(std::string(name));
    return (it != m_root.end() && it->is_object()) ? *it : kEmpty;
}

nlohmann::json &EnvironmentConfig::Edit(std::string_view name)
{
    m_dirty = true;
    nlohmann::json &s = m_root[std::string(name)];
    if (!s.is_object())
        s = nlohmann::json::object();
    return s;
}

bool EnvironmentConfig::Save()
{
    if (!m_dirty)
        return true;
    // The asset file holds the factory defaults (and ends up in assets.blob): only the runtime sections
    // are written, into settings.json next to the player's options (which are left as they are)
    const std::string path = Settings::DefaultPath();
    nlohmann::json settings = JsonFile::ReadObjectFromFileName(path);
    nlohmann::json runtime = nlohmann::json::object();
    for (const char *name : kRuntimeSections)
    {
        if (m_root.contains(name))
            runtime[name] = m_root[name];
    }
    settings[kSettingsKey] = runtime;
    if (!JsonFile::WriteAtomic(path, settings))
    {
        std::cerr << "[Environment] Could not write " << path << std::endl;
        return false;
    }
    m_dirty = false;
    return true;
}

} // namespace OpenArtOfWar
