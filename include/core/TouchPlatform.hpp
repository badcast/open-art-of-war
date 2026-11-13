#pragma once

namespace OpenArtOfWar::TouchPlatform
{
// Platform facilities used by the touch build; unavailable facilities return no path.
void ConfigureBackButton();
const char *ExternalStoragePath();
} // namespace OpenArtOfWar::TouchPlatform
