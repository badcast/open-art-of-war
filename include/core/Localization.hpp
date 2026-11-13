#pragma once

#include "core/LoaderResources.hpp"

#include <string>
#include <string_view>

namespace OpenArtOfWar
{

class Localization
{
public:
    static void Init(
        std::string_view localeDir = {}, std::string_view defaultLang = {}, std::string_view domain = "openaow");
    static void SetLanguage(std::string_view langCode);
    static const std::string &GetLanguage();
    static const std::string &GetDomain();
    static void ToggleLanguage();
    static bool IsRussian();

    static const char *Tr(const char *msgid);
    static std::string Tr(const std::string &msgid);

private:
    static std::string s_currentLanguage;
    static std::string s_localeDir;
    static std::string s_domain;
    static bool s_initialized;
};

} // namespace OpenArtOfWar

#ifndef _
#define _(String) ::OpenArtOfWar::Localization::Tr(String)
#endif
