#include "core/Localization.hpp"
#include "core/AssetStore.hpp"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace OpenArtOfWar
{

std::string Localization::s_currentLanguage = "en";
std::string Localization::s_localeDir;
std::string Localization::s_domain = "openaow";
bool Localization::s_initialized = false;

namespace
{

std::mutex s_mutex;
std::unordered_map<std::string, std::string> s_translations;

bool LoadYamlFile(const std::string &path)
{
    std::string text;
    if (!Assets::ReadText(path, text) || text.empty())
    {
        // Try fallback with assets/ prefix if needed
        if (path.compare(0, 7, "assets/") != 0 && Assets::ReadText("assets/" + path, text) && !text.empty())
        {
            // Successfully read with assets/ prefix
        }
        else
        {
            // Direct disk read as fallback for standalone tools / absolute paths
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec))
            {
                std::ifstream fin(path, std::ios::binary);
                if (fin)
                {
                    text.assign((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
                }
            }
        }
    }

    if (text.empty())
        return false;

    try
    {
        YAML::Node root = YAML::Load(text);
        if (!root.IsMap())
            return false;

        for (auto it = root.begin(); it != root.end(); ++it)
        {
            if (!it->first.IsScalar())
                continue;
            std::string key = it->first.as<std::string>();
            std::string val;
            if (it->second.IsScalar())
                val = it->second.as<std::string>();
            if (!key.empty())
            {
                s_translations[std::move(key)] = std::move(val);
            }
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Localization] Failed to parse " << path << ": " << e.what() << std::endl;
        return false;
    }
}

void ReloadTranslations(const std::string &localeDir, const std::string &domain, const std::string &lang)
{
    s_translations.clear();

    // English is native in code: no catalog needed
    if (lang == "en")
    {
        std::cout << "[Localization] Language 'en' active (native code strings)" << std::endl;
        return;
    }

    std::filesystem::path dir = localeDir.empty() ? "assets/locale" : localeDir;

    // 1. Language subfolder: assets/locale/<lang>/openaow.yaml (or <domain>.yaml)
    const std::string domainName = domain.empty() ? "openaow" : domain;
    LoadYamlFile((dir / lang / (domainName + ".yaml")).generic_string());
    if (domainName != "openaow")
    {
        // Fallback to base openaow.yaml for common strings
        LoadYamlFile((dir / lang / "openaow.yaml").generic_string());
    }

    // Load any other .yaml in assets/locale/<lang>/ (e.g. additional catalogs or mod files)
    const std::string langSubdir = (dir / lang).generic_string();
    for (const std::string &path : Assets::List(langSubdir, ".yaml"))
    {
        LoadYamlFile(path);
    }

    // 2. Fallbacks for flat layout: assets/locale/<lang>.yaml or openaow_<lang>.yaml
    LoadYamlFile((dir / (lang + ".yaml")).generic_string());
    LoadYamlFile((dir / ("openaow_" + lang + ".yaml")).generic_string());

    // 3. Active mods providing translation files in locale root directory:
    for (const std::string &path : Assets::List(dir.generic_string(), ".yaml"))
    {
        const std::string filename = std::filesystem::path(path).filename().string();
        const std::string suffix = "_" + lang + ".yaml";
        if (filename.size() >= suffix.size()
            && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            LoadYamlFile(path);
        }
    }

    std::cout << "[Localization] Loaded " << s_translations.size() << " translations for '" << lang
              << "' (domain: " << domain << ")" << std::endl;
}

} // namespace

void Localization::Init(const std::string &localeDir, const std::string &defaultLang, const std::string &domain)
{
    std::lock_guard<std::mutex> guard(s_mutex);

    s_localeDir = localeDir.empty() ? LoaderResources::LocaleDir() : localeDir;
    s_domain = domain.empty() ? "openaow" : domain;
    s_currentLanguage = (defaultLang == "ru") ? "ru" : "en";
    s_initialized = true;

    ReloadTranslations(s_localeDir, s_domain, s_currentLanguage);
}

void Localization::SetLanguage(const std::string &langCode)
{
    std::lock_guard<std::mutex> guard(s_mutex);
    s_currentLanguage = (langCode == "ru") ? "ru" : "en";
    if (s_initialized)
    {
        ReloadTranslations(s_localeDir, s_domain, s_currentLanguage);
    }
}

const std::string &Localization::GetLanguage()
{
    std::lock_guard<std::mutex> guard(s_mutex);
    return s_currentLanguage;
}

const std::string &Localization::GetDomain()
{
    std::lock_guard<std::mutex> guard(s_mutex);
    return s_domain;
}

void Localization::ToggleLanguage()
{
    std::string next;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        next = (s_currentLanguage == "en") ? "ru" : "en";
    }
    SetLanguage(next);
}

bool Localization::IsRussian()
{
    std::lock_guard<std::mutex> guard(s_mutex);
    return s_currentLanguage == "ru";
}

const char *Localization::Tr(const char *msgid)
{
    if (!msgid || !msgid[0])
        return "";

    // English is the source language in the codebase: return immediately
    if (s_currentLanguage == "en")
        return msgid;

    std::lock_guard<std::mutex> guard(s_mutex);
    auto it = s_translations.find(msgid);
    if (it != s_translations.end() && !it->second.empty())
    {
        return it->second.c_str();
    }
    return msgid;
}

std::string Localization::Tr(const std::string &msgid)
{
    if (msgid.empty())
        return "";
    return std::string(Tr(msgid.c_str()));
}

} // namespace OpenArtOfWar
