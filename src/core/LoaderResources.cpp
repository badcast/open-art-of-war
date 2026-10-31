#include "core/LoaderResources.hpp"

#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <mutex>

namespace OpenArtOfWar
{

    namespace
    {
        // The layout of the shipped assets: used when main.json is missing or lacks a key
        const char *kBuiltinManifest = R"({
            "version": 1,
            "locale": { "dir": "locale", "default_language": "en" },
            "environment": "data/game_environment.json",
            "ui_theme": "characters/game_ui.yaml",
            "fonts": { "regular": "fonts/Roboto-Regular.ttf", "bold": "fonts/Roboto-Bold.ttf" },
            "sprites": { "frames": "characters/sprites.json", "sheet_dir": "sprites", "sheet_prefix": "sheet_", "sheet_ext": ".png", "sheet_count": 39 },
            "ui_textures": { "logo": "ui/logo.png", "title": "ui/title.png" },
            "graphics_data": "data/graphics_data.json",
            "audio": { "dir": "audio", "tracklist": "data/tracklist.json", "license": "audio/LICENSE" },
            "sfx": { "config": "data/sfx.json", "dir": "audio/sfx", "credits": "audio/sfx/CREDITS.txt" },
            "voices": { "config": "data/voices.json", "dir": "audio/voices", "fallback_language": "en" },
            "characters": { "dir": "characters", "extensions": [".yaml", ".yml"],
                            "exclude": ["difficulty.yaml", "ai_difficulty.yaml", "effects.yaml", "projectiles.yaml", "game_ui.yaml",
                                        "research_confederation.yaml", "research_resistance.yaml"] },
            "ai_difficulty": "characters/difficulty.yaml",
            "effects": "characters/effects.yaml",
            "projectiles": "characters/projectiles.yaml",
            "research": { "confederation": "characters/research_confederation.yaml", "resistance": "characters/research_resistance.yaml" },
            "tile_collision": "data/tile_collision.json",
            "net_hash_dirs": [ { "dir": "data", "ext": ".json" }, { "dir": "characters", "ext": ".yaml" }, { "dir": "characters", "ext": ".yml" } ],
            "maps": { "dir": "maps", "ext": ".json", "test_map": "testmap", "net_default": "m26" }
        })";

        struct State
        {
            std::once_flag loaded;
            std::mutex lock;
            nlohmann::json doc;
            std::string text;
            bool fromFile = false;
        };

        State &state()
        {
            static State s;
            return s;
        }

        // Keys of `over` replace those of `base`, objects are merged key by key
        void overlay(nlohmann::json &base, const nlohmann::json &over)
        {
            for(auto it = over.begin(); it != over.end(); ++it)
            {
                if(it.value().is_object() && base.contains(it.key()) && base[it.key()].is_object())
                    overlay(base[it.key()], it.value());
                else
                    base[it.key()] = it.value();
            }
        }

        bool loadInto(State &s, const std::string &path)
        {
            nlohmann::json doc = nlohmann::json::parse(kBuiltinManifest);
            std::string text;
            bool ok = false;
            if(Assets::readText(path, text))
            {
                nlohmann::json file = JsonFile::readFromString(text);
                if(file.is_object() && !file.empty())
                {
                    overlay(doc, file);
                    ok = true;
                }
                else
                    std::cerr << "[LoaderResources] " << path << " is not a JSON object, using the built-in layout" << std::endl;
            }
            else
                std::cerr << "[LoaderResources] " << path << " not found, using the built-in layout" << std::endl;
            std::lock_guard<std::mutex> guard(s.lock);
            s.doc = std::move(doc);
            s.text = ok ? text : std::string();
            s.fromFile = ok;
            return ok;
        }

        void ensureLoaded()
        {
            State &s = state();
            std::call_once(s.loaded, [&] { loadInto(s, LoaderResources::kManifestPath); });
        }

        std::string withExt(const std::string &name, const std::string &ext)
        {
            if(ext.empty() || (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0))
                return name;
            return name + ext;
        }
    } // namespace

    bool LoaderResources::init(const std::string &manifestPath)
    {
        State &s = state();
        std::call_once(s.loaded, [] {}); // later lazy loads must not overwrite this one
        return loadInto(s, manifestPath);
    }

    bool LoaderResources::hasManifest()
    {
        ensureLoaded();
        return state().fromFile;
    }

    const std::string &LoaderResources::manifestText()
    {
        ensureLoaded();
        return state().text;
    }

    const nlohmann::json &LoaderResources::manifest()
    {
        ensureLoaded();
        return state().doc;
    }

    std::string LoaderResources::join(const std::string &root, const std::string &relative)
    {
        if(root.empty())
            return relative;
        if(relative.empty())
            return root;
        if(root.back() == '/' || root.back() == '\\')
            return root + relative;
        return root + "/" + relative;
    }

    std::string LoaderResources::str(const char *section, const char *key, const char *fallback)
    {
        const nlohmann::json &m = manifest();
        const nlohmann::json *node = &m;
        if(section)
        {
            auto it = m.find(section);
            if(it == m.end() || !it->is_object())
                return fallback;
            node = &*it;
        }
        auto it = node->find(key);
        if(it == node->end() || !it->is_string())
            return fallback;
        return it->get<std::string>();
    }

    std::string LoaderResources::localeDir(const std::string &root)
    {
        return join(root, str("locale", "dir", "locale"));
    }

    std::string LoaderResources::defaultLanguage()
    {
        return str("locale", "default_language", "en");
    }

    std::string LoaderResources::environment(const std::string &root)
    {
        return join(root, str(nullptr, "environment", "data/game_environment.json"));
    }

    std::string LoaderResources::uiTheme(const std::string &root)
    {
        return join(root, str(nullptr, "ui_theme", "characters/game_ui.yaml"));
    }

    std::string LoaderResources::fontRegular(const std::string &root)
    {
        return join(root, str("fonts", "regular", "fonts/Roboto-Regular.ttf"));
    }

    std::string LoaderResources::fontBold(const std::string &root)
    {
        return join(root, str("fonts", "bold", "fonts/Roboto-Bold.ttf"));
    }

    std::string LoaderResources::spriteFrames(const std::string &root)
    {
        return join(root, str("sprites", "frames", "characters/sprites.json"));
    }

    int LoaderResources::spriteSheetCount()
    {
        const nlohmann::json &m = manifest();
        auto it = m.find("sprites");
        if(it != m.end() && it->is_object())
        {
            auto c = it->find("sheet_count");
            if(c != it->end() && c->is_number_integer())
                return std::clamp(c->get<int>(), 0, 1000);
        }
        return 39;
    }

    std::string LoaderResources::spriteSheet(int index, const std::string &root)
    {
        char num[16];
        std::snprintf(num, sizeof(num), "%02d", index);
        const std::string dir = str("sprites", "sheet_dir", "sprites");
        return join(join(root, dir), str("sprites", "sheet_prefix", "sheet_") + num + str("sprites", "sheet_ext", ".png"));
    }

    std::vector<std::pair<std::string, std::string>> LoaderResources::uiTextures(const std::string &root)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const nlohmann::json &m = manifest();
        auto it = m.find("ui_textures");
        if(it == m.end() || !it->is_object())
            return out;
        for(auto e = it->begin(); e != it->end(); ++e)
        {
            if(e.value().is_string())
                out.emplace_back(e.key(), join(root, e.value().get<std::string>()));
        }
        return out;
    }

    std::string LoaderResources::graphicsData(const std::string &root)
    {
        return join(root, str(nullptr, "graphics_data", "data/graphics_data.json"));
    }

    std::string LoaderResources::audioDir(const std::string &root)
    {
        return join(root, str("audio", "dir", "audio"));
    }

    std::string LoaderResources::audioFile(const std::string &name, const std::string &root)
    {
        return join(audioDir(root), name);
    }

    std::string LoaderResources::tracklist(const std::string &root)
    {
        return join(root, str("audio", "tracklist", "data/tracklist.json"));
    }

    std::string LoaderResources::audioLicense(const std::string &root)
    {
        return join(root, str("audio", "license", "audio/LICENSE"));
    }

    std::string LoaderResources::sfxConfig(const std::string &root)
    {
        return join(root, str("sfx", "config", "data/sfx.json"));
    }

    std::string LoaderResources::voicesConfig(const std::string &root)
    {
        return join(root, str("voices", "config", "data/voices.json"));
    }

    std::string LoaderResources::voicesDir(const std::string &language, const std::string &root)
    {
        return join(join(root, str("voices", "dir", "audio/voices")), language);
    }

    std::string LoaderResources::voicesFallbackLanguage()
    {
        return str("voices", "fallback_language", "en");
    }

    std::string LoaderResources::sfxDir(const std::string &root)
    {
        return join(root, str("sfx", "dir", "audio/sfx"));
    }

    std::string LoaderResources::sfxCredits(const std::string &root)
    {
        return join(root, str("sfx", "credits", "audio/sfx/CREDITS.txt"));
    }

    std::string LoaderResources::aiDifficulty(const std::string &root)
    {
        return join(root, str(nullptr, "ai_difficulty", "characters/difficulty.yaml"));
    }

    std::string LoaderResources::effects(const std::string &root)
    {
        return join(root, str(nullptr, "effects", "characters/effects.yaml"));
    }

    std::string LoaderResources::projectiles(const std::string &root)
    {
        return join(root, str(nullptr, "projectiles", "characters/projectiles.yaml"));
    }

    std::string LoaderResources::research(const std::string &faction, const std::string &root)
    {
        const std::string fallback = "characters/research_" + faction + ".yaml";
        return join(root, str("research", faction.c_str(), fallback.c_str()));
    }

    std::string LoaderResources::tileCollision(const std::string &root)
    {
        return join(root, str(nullptr, "tile_collision", "data/tile_collision.json"));
    }

    std::string LoaderResources::charactersDir(const std::string &root)
    {
        return join(root, str("characters", "dir", "characters"));
    }

    std::vector<std::string> LoaderResources::characterExtensions()
    {
        std::vector<std::string> out;
        const nlohmann::json &m = manifest();
        auto it = m.find("characters");
        if(it != m.end() && it->is_object())
        {
            auto e = it->find("extensions");
            if(e != it->end() && e->is_array())
            {
                for(const auto &v : *e)
                {
                    if(v.is_string())
                        out.push_back(v.get<std::string>());
                }
            }
        }
        if(out.empty())
            out = {".yaml", ".yml"};
        return out;
    }

    bool LoaderResources::isCharacterExcluded(const std::string &fileName)
    {
        const nlohmann::json &m = manifest();
        auto it = m.find("characters");
        if(it == m.end() || !it->is_object())
            return false;
        auto e = it->find("exclude");
        if(e == it->end() || !e->is_array())
            return false;
        for(const auto &v : *e)
        {
            if(v.is_string() && v.get<std::string>() == fileName)
                return true;
        }
        return false;
    }

    std::vector<std::string> LoaderResources::characterFiles(const std::string &root)
    {
        std::vector<std::string> files;
        const std::string dir = charactersDir(root);
        for(const std::string &ext : characterExtensions())
        {
            for(std::string &f : Assets::list(dir, ext))
            {
                const auto slash = f.find_last_of("/\\");
                const std::string name = slash == std::string::npos ? f : f.substr(slash + 1);
                if(!isCharacterExcluded(name))
                    files.push_back(std::move(f));
            }
        }
        return files;
    }

    std::string LoaderResources::mapsDir(const std::string &root)
    {
        return join(root, str("maps", "dir", "maps"));
    }

    std::string LoaderResources::mapExtension()
    {
        return str("maps", "ext", ".json");
    }

    std::string LoaderResources::mapPath(const std::string &name, const std::string &root)
    {
        if(name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
            return withExt(name, mapExtension());
        return join(mapsDir(root), withExt(name, mapExtension()));
    }

    std::string LoaderResources::testMap(const std::string &root)
    {
        return mapPath(str("maps", "test_map", "testmap"), root);
    }

    std::string LoaderResources::netDefaultMap(const std::string &root)
    {
        return mapPath(str("maps", "net_default", "m26"), root);
    }

    std::vector<std::string> LoaderResources::listMaps(const std::string &root)
    {
        return Assets::list(mapsDir(root), mapExtension());
    }

    std::vector<std::string> LoaderResources::netHashFiles(const std::string &root)
    {
        std::vector<std::string> files;
        const nlohmann::json &m = manifest();
        auto it = m.find("net_hash_dirs");
        if(it != m.end() && it->is_array())
        {
            for(const auto &d : *it)
            {
                if(!d.is_object() || !d.contains("dir") || !d["dir"].is_string())
                    continue;
                const std::string ext = d.value("ext", std::string());
                const std::string sfx = sfxConfig(root);
                const std::string voices = voicesConfig(root);
                for(std::string &f : Assets::list(join(root, d["dir"].get<std::string>()), ext))
                {
                    // Sound and voice settings do not change the simulation: different ones must not keep players apart
                    if(f != sfx && f != voices)
                        files.push_back(std::move(f));
                }
            }
        }
        return files;
    }

} // namespace OpenArtOfWar
