// Mods: finding them (folders and .mod archives, one per id), putting their files over the installed data, the manifest
// merge, the order of the layers and the paths a mod may not reach
#include "core/AssetStore.hpp"
#include "core/Mods.hpp"
#include "core/Sha256.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OpenArtOfWar;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if (!(c))                                                   \
        {                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

static void Write(const fs::path &p, const std::string &text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A plain (uncompressed) ustar archive: what libarchive reads besides tar.zst
static void WriteTar(const fs::path &p, const std::vector<std::pair<std::string, std::string>> &files)
{
    std::string out;
    for (const auto &[name, data] : files)
    {
        char h[512] = {};
        std::snprintf(h, 100, "%s", name.c_str());
        std::snprintf(h + 100, 8, "%07o", 0644);
        std::snprintf(h + 108, 8, "%07o", 0);
        std::snprintf(h + 116, 8, "%07o", 0);
        std::snprintf(h + 124, 12, "%011o", static_cast<unsigned>(data.size()));
        std::snprintf(h + 136, 12, "%011o", 0);
        h[156] = '0';
        std::memcpy(h + 257, "ustar", 5);
        std::memcpy(h + 263, "00", 2);
        std::memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (unsigned char c : h)
            sum += c;
        std::snprintf(h + 148, 8, "%06o", sum);
        h[155] = ' ';
        out.append(h, 512);
        out += data;
        out.append((512 - data.size() % 512) % 512, '\0');
    }
    out.append(1024, '\0');
    Write(p, out);
}

int main()
{
    const fs::path root = fs::temp_directory_path() / ("openaow_mods_test_" + std::to_string(SDL_GetTicksNS()));
    const fs::path app = root / "app_mods", user = root / "user_mods";
    Mods::SetSearchDirs(app.string(), user.string());

    // alpha: a folder in the user directory
    Write(user / "alpha" / "mod.yaml", R"(id: alpha
name: Alpha
version: "1.0"
author: me
manifest:
  maps:
    skirmish: ["maps/alpha_map.json"]
  title: Alpha game
)");
    Write(user / "alpha" / "data" / "x.yaml", "from: alpha\n");
    Write(user / "alpha" / "sprites" / "sprite_alpha_meta.yaml", "frames: {}\n");
    Write(user / "alpha" / "preview.png", "PNG_ALPHA_PREVIEW");
    // beta: 1.0 next to the game and in the user directory (the user's counts), both bring data/x.yaml
    WriteTar(app / "beta.mod", {{"mod.yaml", "id: beta\nversion: \"1.0\"\n"}, {"data/x.yaml", "from: beta app\n"}});
    WriteTar(user / "beta.mod",
        {{"mod.yaml", "id: beta\nversion: \"1.0\"\n"},
            {"data/x.yaml", "from: beta user\n"},
            {"preview.png", "PNG_BETA_PREVIEW"},
            {"assets/data/y.yaml", "y: 1\n"},
            {"../evil.yaml", "no\n"}});
    // gamma: 2.0 next to the game wins over 1.5 of the user
    WriteTar(app / "gamma.mod", {{"mod.yaml", "id: gamma\nversion: \"2.0\"\n"}});
    WriteTar(user / "gamma.mod", {{"mod.yaml", "id: gamma\nversion: \"1.5\"\n"}});
    // broken ones are listed with the reason
    Write(user / "broken" / "mod.yaml", "id: Not Valid\n");

    const std::vector<Mods::ModInfo> found = Mods::Scan();
    auto find = [&](const std::string &id) -> const Mods::ModInfo * {
        for (const auto &m : found)
            if (m.id == id)
                return &m;
        return nullptr;
    };
    CHECK(found.size() == 4);
    CHECK(find("alpha") && find("alpha")->name == "Alpha" && find("alpha")->error.empty() && !find("alpha")->archive);
    CHECK(find("beta") && find("beta")->fromUser && find("beta")->archive);
    CHECK(find("gamma") && find("gamma")->version == "2.0" && !find("gamma")->fromUser);
    CHECK(find("broken") && !find("broken")->error.empty());

    std::vector<std::uint8_t> pv;
    CHECK(Mods::LoadPreview(*find("alpha"), pv) && std::string(pv.begin(), pv.end()) == "PNG_ALPHA_PREVIEW");
    CHECK(Mods::LoadPreview(*find("beta"), pv) && std::string(pv.begin(), pv.end()) == "PNG_BETA_PREVIEW");
    CHECK(!Mods::LoadPreview(*find("gamma"), pv));

    CHECK(Mods::CompareVersions("1.10", "1.9") > 0);
    CHECK(Mods::CompareVersions("1.0", "1.0") == 0);
    CHECK(Mods::CompareVersions("1.0", "1.0.1") < 0);
    std::string key;
    CHECK(Mods::KeyOf("./sprites/../sprites/a.png", key) && key == "sprites/a.png");
    CHECK(Mods::KeyOf("assets/maps/m.json", key) && key == "maps/m.json");
    CHECK(!Mods::KeyOf("../x", key) && !Mods::KeyOf("/etc/passwd", key) && !Mods::KeyOf("C:/x", key)
          && !Mods::KeyOf("a\\b", key));

    // alpha then beta: beta's data/x.yaml wins, alpha's files are there, the escaping entry is refused
    Mods::ActivateReport report;
    CHECK(!Mods::Activate({"alpha", "beta", "missing"}, &report)); // "missing" is reported, the others are in place
    CHECK(Mods::Active().size() == 2);
    std::string text;
    CHECK(Assets::ReadText("assets/data/x.yaml", text) && text == "from: beta user\n");
    CHECK(Assets::ReadText("assets/data/y.yaml", text) && text == "y: 1\n");
    CHECK(Assets::Exists("assets/sprites/sprite_alpha_meta.yaml"));
    CHECK(!Assets::Exists("evil.yaml") && !Assets::Exists("assets/../evil.yaml"));
    CHECK(report.conflicts.count("data/x.yaml") && report.conflicts["data/x.yaml"].size() == 2);
    const std::vector<std::string> listed = Assets::List("assets/sprites", ".yaml");
    CHECK(std::find(listed.begin(), listed.end(), "assets/sprites/sprite_alpha_meta.yaml") != listed.end());
    // the other order: alpha wins
    Mods::Activate({"beta", "alpha"});
    CHECK(Assets::ReadText("assets/data/x.yaml", text) && text == "from: alpha\n");

    // The manifest: maps merge, lists grow without doubles, values are replaced
    nlohmann::json base = nlohmann::json::parse(R"({"title": "Game", "maps": {"skirmish": ["maps/a.json"]}})");
    Mods::MergeManifest(base, find("alpha")->manifest);
    Mods::MergeManifest(base, find("alpha")->manifest);
    CHECK(base["title"] == "Alpha game");
    CHECK(base["maps"]["skirmish"].size() == 2 && base["maps"]["skirmish"][1] == "maps/alpha_map.json");

    // None: the installed data again
    Mods::Deactivate();
    CHECK(Mods::Active().empty() && !Assets::HasModLayers());
    CHECK(!Assets::Exists("assets/sprites/sprite_alpha_meta.yaml"));

    // Multi-blob mounting and integrity check
    {
        std::error_code dirEc;
        const fs::path blobDir = root / "blob_test";
        fs::create_directories(blobDir, dirEc);

        // Blob 1: base files + certificate
        const std::string text1 = "version: 1\n";
        const std::string textCommon = "from: base\n";
        const std::string hash1 = Sha256::Hex(text1.data(), text1.size());
        const std::string hashCommonBase = Sha256::Hex(textCommon.data(), textCommon.size());

        std::string cert1 = "# Certificate 1\n";
        cert1 += "assets/data/v1.yaml  " + hash1 + "\n";
        cert1 += "assets/data/common.yaml  " + hashCommonBase + "\n";

        WriteTar(blobDir / "00_base.blob", {
            {"certificate.sha256", cert1},
            {"data/v1.yaml", text1},
            {"data/common.yaml", textCommon}
        });

        // Blob 2: overrides common.yaml, adds v2.yaml + certificate
        const std::string text2 = "version: 2\n";
        const std::string textCommonPatch = "from: patch\n";
        const std::string hash2 = Sha256::Hex(text2.data(), text2.size());
        const std::string hashCommonPatch = Sha256::Hex(textCommonPatch.data(), textCommonPatch.size());

        std::string cert2 = "# Certificate 2\n";
        cert2 += "assets/data/v2.yaml  " + hash2 + "\n";
        cert2 += "assets/data/common.yaml  " + hashCommonPatch + "\n";

        WriteTar(blobDir / "01_patch.blob", {
            {"certificate.sha256", cert2},
            {"data/v2.yaml", text2},
            {"data/common.yaml", textCommonPatch}
        });

        const fs::path oldCwd = fs::current_path(dirEc);
        fs::current_path(blobDir, dirEc);
        Assets::ResetForTesting();

        CHECK(Assets::Init());
        CHECK(Assets::IsBlob());
        CHECK(Assets::LoadedBlobPaths().size() == 2);

        // Test layered reading
        std::string content;
        CHECK(Assets::ReadText("assets/data/v1.yaml", content) && content == text1);
        CHECK(Assets::ReadText("assets/data/v2.yaml", content) && content == text2);
        CHECK(Assets::ReadText("assets/data/common.yaml", content) && content == textCommonPatch); // Patch won!

        // Test List across blobs
        const std::vector<std::string> blobFiles = Assets::List("assets/data", ".yaml");
        CHECK(blobFiles.size() == 3);

        // Test certificate verification
        std::vector<std::string> problems;
        std::string certPath;
        CHECK(Assets::VerifyCertificate(problems, certPath) && problems.empty());

        // Restore cwd and reset
        fs::current_path(oldCwd, dirEc);
        Assets::ResetForTesting();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
