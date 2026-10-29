#include "core/Settings.hpp"
#include "core/AssetStore.hpp"
#include "core/JsonFile.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

    void Settings::clamp()
    {
        constexpr int MaxResolution = 16384;
        windowMode = static_cast<WindowMode>(std::clamp(static_cast<int>(windowMode), 0, 2));
        windowW = std::clamp(windowW, kMinWindowW, MaxResolution);
        windowH = std::clamp(windowH, kMinWindowH, MaxResolution);
        fullscreenW = std::clamp(fullscreenW, kMinWindowW, MaxResolution);
        fullscreenH = std::clamp(fullscreenH, kMinWindowH, MaxResolution);
        fpsLimit = std::clamp(fpsLimit, 0, 1000);
        graphicsQuality = std::clamp(graphicsQuality, 0, 2);
        colorGrade = std::clamp(colorGrade, 0, 5);
        zoomMin = std::clamp(zoomMin, kZoomLowest, 1.0f);
        zoomMax = std::clamp(zoomMax, 2.0f, kZoomHighest);
        zoomDefault = std::clamp(zoomDefault, zoomMin, zoomMax);
        musicVolume = std::clamp(musicVolume, 0, 128);
        fxVolume = std::clamp(fxVolume, 0, 128);
        difficulty = std::clamp(difficulty, 0, 3);
        if(language != "en" && language != "ru")
            language = "en";
        if(playerName.empty())
            playerName = "Player";
        if(playerName.size() > 24)
            playerName.resize(24);
        if(lastAddress.size() > 120)
            lastAddress.clear();
    }

    std::string Settings::defaultPath()
    {
        return (std::filesystem::path(Assets::userDataDir()) / "settings.json").string();
    }

    bool Settings::save(const std::string &path) const
    {
        try
        {
            // Other parts of the game keep their own keys in this file (e.g. "environment"): leave them alone
            nlohmann::json j = JsonFile::readObject(path);
            j["windowMode"] = static_cast<int>(windowMode);
            j["windowWidth"] = windowW;
            j["windowHeight"] = windowH;
            j["fullscreenWidth"] = fullscreenW;
            j["fullscreenHeight"] = fullscreenH;
            j["vsync"] = vsync;
            j["fpsLimit"] = fpsLimit;
            j["showFps"] = showFps;
            j["graphicsQuality"] = graphicsQuality;
            j["smoothing"] = smoothing;
            j["zoomDefault"] = zoomDefault;
            j["zoomMin"] = zoomMin;
            j["zoomMax"] = zoomMax;
            j["colorGrade"] = colorGrade;
            j["glow"] = glow;
            j["sharpPixels"] = sharpPixels;
            j["musicVolume"] = musicVolume;
            j["fxVolume"] = fxVolume;
            j["muted"] = muted;
            j["language"] = language;
            j["playerName"] = playerName;
            j["lastAddress"] = lastAddress;
            j["difficulty"] = difficulty;

            return JsonFile::writeAtomic(path, j);
        }
        catch(const std::exception &e)
        {
            std::cerr << "[Settings] Save failed: " << e.what() << std::endl;
            return false;
        }
    }

    bool Settings::load(const std::string &path)
    {
        try
        {
            std::ifstream fh(path);
            if(!fh.is_open())
                return false;
            const nlohmann::json j = nlohmann::json::parse(fh, nullptr, false);
            if(!j.is_object())
                return false;

            windowMode = static_cast<WindowMode>(j.value("windowMode", static_cast<int>(windowMode)));
            windowW = j.value("windowWidth", windowW);
            windowH = j.value("windowHeight", windowH);
            fullscreenW = j.value("fullscreenWidth", fullscreenW);
            fullscreenH = j.value("fullscreenHeight", fullscreenH);
            vsync = j.value("vsync", vsync);
            fpsLimit = j.value("fpsLimit", fpsLimit);
            showFps = j.value("showFps", showFps);
            graphicsQuality = j.value("graphicsQuality", graphicsQuality);
            smoothing = j.value("smoothing", smoothing);
            zoomDefault = j.value("zoomDefault", zoomDefault);
            zoomMin = j.value("zoomMin", zoomMin);
            zoomMax = j.value("zoomMax", zoomMax);
            colorGrade = j.value("colorGrade", colorGrade);
            glow = j.value("glow", glow);
            sharpPixels = j.value("sharpPixels", sharpPixels);
            musicVolume = j.value("musicVolume", musicVolume);
            fxVolume = j.value("fxVolume", fxVolume);
            muted = j.value("muted", muted);
            language = j.value("language", language);
            playerName = j.value("playerName", playerName);
            lastAddress = j.value("lastAddress", lastAddress);
            difficulty = j.value("difficulty", difficulty);
            clamp();
            return true;
        }
        catch(const std::exception &e)
        {
            std::cerr << "[Settings] Load failed: " << e.what() << std::endl;
            return false;
        }
    }

} // namespace OpenArtOfWar
