#pragma once

#include <string>

namespace OpenArtOfWar
{

    class Localization
    {
    public:
        static void init(const std::string &localeDir = "assets/locale", const std::string &defaultLang = "en");
        static void setLanguage(const std::string &langCode);
        static const std::string &getLanguage();
        static void toggleLanguage();
        static bool isRussian();

        static const char *tr(const char *msgid);
        static std::string tr(const std::string &msgid);

    private:
        static std::string s_currentLanguage;
        static std::string s_localeDir;
        static bool s_initialized;
    };

} // namespace OpenArtOfWar

#ifndef _
#define _(String) ::OpenArtOfWar::Localization::tr(String)
#endif
