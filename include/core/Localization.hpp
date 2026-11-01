#pragma once

#include "core/LoaderResources.hpp"

#include <string>

namespace OpenArtOfWar
{

    class Localization
    {
    public:
        static void Init(const std::string &localeDir = LoaderResources::LocaleDir(), const std::string &defaultLang = LoaderResources::DefaultLanguage());
        static void SetLanguage(const std::string &langCode);
        static const std::string &GetLanguage();
        static void ToggleLanguage();
        static bool IsRussian();

        static const char *Tr(const char *msgid);
        static std::string Tr(const std::string &msgid);

    private:
        static std::string s_currentLanguage;
        static std::string s_localeDir;
        static bool s_initialized;
    };

} // namespace OpenArtOfWar

#ifndef _
#define _(String) ::OpenArtOfWar::Localization::Tr(String)
#endif
