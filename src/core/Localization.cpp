#include "core/Localization.hpp"
#include "core/AssetStore.hpp"
#include <libintl.h>
#include <locale.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#if defined(__GLIBC__)
extern "C" int _nl_msg_cat_cntr;
#endif

namespace OpenArtOfWar
{

std::string Localization::s_currentLanguage = "en";
std::string Localization::s_localeDir; // set by init() from LoaderResources::localeDir()
std::string Localization::s_domain = "openaow";
bool Localization::s_initialized = false;

void Localization::Init(const std::string &localeDir, const std::string &defaultLang, const std::string &domain)
{
    std::filesystem::path dir(Assets::RealDirectory(localeDir));
    if (!std::filesystem::exists(dir))
    {
        if (std::filesystem::exists("../" + localeDir))
        {
            dir = "../" + localeDir;
        }
        else if (std::filesystem::exists("locale"))
        {
            dir = "locale";
        }
    }

    try
    {
        if (std::filesystem::exists(dir))
        {
            s_localeDir = std::filesystem::absolute(dir).string();
        }
        else
        {
            s_localeDir = localeDir;
        }
    }
    catch (...)
    {
        s_localeDir = localeDir;
    }

    s_domain = domain.empty() ? "openaow" : domain;

    setlocale(LC_ALL, "");
#if defined(LC_MESSAGES)
    setlocale(LC_MESSAGES, "");
#endif

    bindtextdomain(s_domain.c_str(), s_localeDir.c_str());
    bind_textdomain_codeset(s_domain.c_str(), "UTF-8");
    textdomain(s_domain.c_str());

    if (s_domain != "openaow")
    {
        bindtextdomain("openaow", s_localeDir.c_str());
        bind_textdomain_codeset("openaow", "UTF-8");
    }

    s_initialized = true;
    SetLanguage(defaultLang.empty() ? "en" : defaultLang);
    std::cout << "[Localization] Initialized gettext domain '" << s_domain << "' at " << s_localeDir
              << ", default language: " << s_currentLanguage << std::endl;
}

void Localization::SetLanguage(const std::string &langCode)
{
    s_currentLanguage = (langCode == "ru") ? "ru" : "en";
#if defined(_WIN32)
    _putenv_s("LANGUAGE", s_currentLanguage.c_str());
#else
    setenv("LANGUAGE", s_currentLanguage.c_str(), 1);
#endif

#if defined(__GLIBC__)
    ++_nl_msg_cat_cntr; // Invalidate glibc gettext cache
#endif

    const char *localeStr = (s_currentLanguage == "ru") ? "ru_RU.UTF-8" : "en_US.UTF-8";
#if defined(LC_MESSAGES)
    setlocale(LC_MESSAGES, localeStr);
#else
    (void) localeStr; // gettext reads LANGUAGE on platforms without LC_MESSAGES
#endif
}

const std::string &Localization::GetLanguage()
{
    return s_currentLanguage;
}

const std::string &Localization::GetDomain()
{
    return s_domain;
}

void Localization::ToggleLanguage()
{
    if (s_currentLanguage == "en")
    {
        SetLanguage("ru");
    }
    else
    {
        SetLanguage("en");
    }
}

bool Localization::IsRussian()
{
    return s_currentLanguage == "ru";
}

const char *Localization::Tr(const char *msgid)
{
    if (!msgid || !msgid[0])
        return "";
    const char *translation = gettext(msgid);
    if (translation == msgid && s_domain != "openaow")
    {
        translation = dgettext("openaow", msgid);
    }
    return translation;
}

std::string Localization::Tr(const std::string &msgid)
{
    if (msgid.empty())
        return "";
    return std::string(Tr(msgid.c_str()));
}

} // namespace OpenArtOfWar
