#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Read access to the game assets, either from an "assets" directory or from a "data.blob" (tar.zst) that
// holds the contents of that directory. The directory wins: it is searched for first, so a checkout
// or a modded install keeps working, and data.blob is the packed fallback for distribution.
//
// Every path is written as it is on disk ("assets/maps/m0.json", "../assets/fonts/x.ttf"); in blob
// mode the leading "assets" component selects the entry inside the archive.
// The blob is unpacked into memory on start-up (a tar stream cannot be read at random). The blob is read-only:
// anything the game or the editors write goes to a per-user directory (writablePath) which is also
// searched before the blob, so a saved map shadows the packed one.
namespace OpenArtOfWar::Assets
{

    enum class Mode
    {
        None,
        Directory,
        Blob
    };

    // Locates the assets (working directory, then next to the executable, one level up and in
    // ../share/openaow). Switches the working directory to the folder that holds "assets" when found.
    // Safe to call more than once.
    bool init();

    Mode mode();
    inline bool isBlob()
    {
        return mode() == Mode::Blob;
    }
    std::string describe();

    bool exists(const std::string &path);
    bool readBytes(const std::string &path, std::vector<uint8_t> &out);
    bool readText(const std::string &path, std::string &out);

    // Regular files directly inside `dir`, sorted, as "<dir>/<name>"; `extension` is e.g. ".json" (empty = all)
    std::vector<std::string> list(const std::string &dir, const std::string &extension = "");

    // Where to write `path`: unchanged in directory mode, below the per-user directory in blob mode.
    // Parent directories are created.
    std::string writablePath(const std::string &path);

    // A directory on disk with the contents of `dir` (for libraries that need real files, like gettext):
    // `dir` itself in directory mode, an extracted copy in the per-user cache in blob mode.
    std::string realDirectory(const std::string &dir);

    // Per-user directory for settings, saves and caches: SDL_GetPrefPath("openaow", "openaow"), i.e. below
    // ~/.local/share (Linux), %APPDATA% (Windows) or ~/Library/Application Support (macOS)
    std::string userDataDir();
    // Replaces that directory (tests use a temporary one); call before anything reads or writes there
    void setUserDataDir(const std::string &dir);

} // namespace OpenArtOfWar::Assets
