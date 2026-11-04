#pragma once

#include <cstdint>
#include <map>
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
bool Init();

Mode GetMode();
inline Mode mode()
{
    return GetMode();
}
inline bool IsBlob()
{
    return GetMode() == Mode::Blob;
}
std::string Describe();

// The integrity certificate of a blob: an entry of the blob itself, written by tools/pack_assets.py, one
// "<path>  <sha256>" line per packed file
inline constexpr const char *kCertificateName = "certificate.sha256";

// Checks a loaded blob against the certificate it carries: the certificate must be there, every file it lists must
// be in the blob with that SHA-256, and the blob must hold no file the certificate does not list. The packed files
// are checked, never a per-user copy. A plain assets directory has no certificate and is not checked (true).
// False with a readable report in `problems` (one line per file); `certificatePath` names the blob's certificate.
bool VerifyCertificate(std::vector<std::string> &problems, std::string &certificatePath);

// A session overlay: data files (key "characters/x.yaml") that win over the installed ones until it is cleared. It
// lives in memory only (never in assets/ or the user directory), so it ends with the game; the network game uses it to
// play with the host's simulation data. The keys are paths below assets/.
void SetSessionOverlay(std::map<std::string, std::vector<uint8_t>> files);
void ClearSessionOverlay();
bool HasSessionOverlay();

bool Exists(const std::string &path);
bool ReadBytes(const std::string &path, std::vector<uint8_t> &out);
bool ReadText(const std::string &path, std::string &out);

// Regular files directly inside `dir`, sorted, as "<dir>/<name>"; `extension` is e.g. ".json" (empty = all)
std::vector<std::string> List(const std::string &dir, const std::string &extension = "");

// Where to write `path`: unchanged in directory mode, below the per-user directory in blob mode.
// Parent directories are created.
std::string WritablePath(const std::string &path);

// A directory on disk with the contents of `dir` (for libraries that need real files, like gettext):
// `dir` itself in directory mode, an extracted copy in the per-user cache in blob mode.
std::string RealDirectory(const std::string &dir);

// Per-user directory for settings, saves and caches: SDL_GetPrefPath("openaow", "openaow"), i.e. below
// ~/.local/share (Linux), %APPDATA% (Windows) or ~/Library/Application Support (macOS)
std::string UserDataDir();
// Replaces that directory (tests use a temporary one); call before anything reads or writes there
void SetUserDataDir(const std::string &dir);

} // namespace OpenArtOfWar::Assets
