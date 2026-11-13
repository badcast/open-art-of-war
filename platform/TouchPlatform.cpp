#include "core/TouchPlatform.hpp"
#include <SDL3/SDL.h>

namespace OpenArtOfWar::TouchPlatform
{
void ConfigureBackButton()
{
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
}

const char *ExternalStoragePath()
{
#if defined(__ANDROID__)
    return SDL_GetAndroidExternalStoragePath();
#else
    return nullptr;
#endif
}
}
