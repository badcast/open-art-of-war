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
    std::string Localization::s_localeDir = "assets/locale";
    bool Localization::s_initialized = false;

    void Localization::init(const std::string &localeDir, const std::string &defaultLang)
    {
        std::filesystem::path dir(Assets::realDirectory(localeDir));
        if(!std::filesystem::exists(dir))
        {
            if(std::filesystem::exists("../" + localeDir))
            {
                dir = "../" + localeDir;
            }
            else if(std::filesystem::exists("locale"))
            {
                dir = "locale";
            }
        }

        try
        {
            if(std::filesystem::exists(dir))
            {
                s_localeDir = std::filesystem::absolute(dir).string();
            }
            else
            {
                s_localeDir = localeDir;
            }
        }
        catch(...)
        {
            s_localeDir = localeDir;
        }

        setlocale(LC_ALL, "");
#if defined(LC_MESSAGES)
        setlocale(LC_MESSAGES, "");
#endif

        bindtextdomain("openaow", s_localeDir.c_str());
        bind_textdomain_codeset("openaow", "UTF-8");
        textdomain("openaow");

        s_initialized = true;
        setLanguage(defaultLang.empty() ? "en" : defaultLang);
        std::cout << "[Localization] Initialized gettext domain 'openaow' at " << s_localeDir << ", default language: " << s_currentLanguage << std::endl;
    }

    void Localization::setLanguage(const std::string &langCode)
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

    const std::string &Localization::getLanguage()
    {
        return s_currentLanguage;
    }

    void Localization::toggleLanguage()
    {
        if(s_currentLanguage == "en")
        {
            setLanguage("ru");
        }
        else
        {
            setLanguage("en");
        }
    }

    bool Localization::isRussian()
    {
        return s_currentLanguage == "ru";
    }

    const char *Localization::tr(const char *msgid)
    {
        if(!msgid || !msgid[0])
            return "";
        return gettext(msgid);
    }

    std::string Localization::tr(const std::string &msgid)
    {
        if(msgid.empty())
            return "";
        return std::string(gettext(msgid.c_str()));
    }

} // namespace OpenArtOfWar
