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

void Settings::Clamp()
{
    constexpr int MaxResolution = 16384;
    windowMode = static_cast<WindowMode>(std::clamp(static_cast<int>(windowMode), 0, 2));
    windowW = std::clamp(windowW, kMinWindowW, MaxResolution);
    windowH = std::clamp(windowH, kMinWindowH, MaxResolution);
    fullscreenW = std::clamp(fullscreenW, kMinWindowW, MaxResolution);
    fullscreenH = std::clamp(fullscreenH, kMinWindowH, MaxResolution);
    fpsLimit = std::clamp(fpsLimit, 0, 1000);
    graphicsQuality = std::clamp(graphicsQuality, 0, 2);
    // (dusk and night are no longer a player's choice but the time of day of the match: such a setting is off)
    colorGrade = colorGrade > 3 ? 0 : std::max(colorGrade, 0); // 3: ColorGrade::Cinematic
    zoomMin = std::clamp(zoomMin, kZoomLowest, 1.0f);
    zoomMax = std::clamp(zoomMax, 2.0f, kZoomHighest);
    zoomDefault = std::clamp(zoomDefault, zoomMin, zoomMax);
    generalVolume = std::clamp(generalVolume, 0, 128);
    musicVolume = std::clamp(musicVolume, 0, 128);
    fxVolume = std::clamp(fxVolume, 0, 128);
    voiceVolume = std::clamp(voiceVolume, 0, 128);
    ambientVolume = std::clamp(ambientVolume, 0, 128);
    uiVolume = std::clamp(uiVolume, 0, 128);
    difficulty = std::clamp(difficulty, 0, 3);
    edgePanMargin = std::clamp(edgePanMargin, kMinEdgePanMargin, kMaxEdgePanMargin);
    if (language != "en" && language != "ru")
        language = "en";
    if (playerName.empty())
        playerName = DefaultUserName();
    if (playerName.size() > 24)
        playerName.resize(24);
    if (lastAddress.size() > 120)
        lastAddress.clear();
}

std::string Settings::DefaultPath()
{
    return (std::filesystem::path(Assets::UserDataDir()) / "settings.json").string();
}

std::string Settings::DefaultUserName()
{
    std::string name = std::getenv("USER") ? std::getenv("USER") : "AOW Player";
    if (name.size() > 24)
        name.resize(24);
    return name;
}

bool Settings::Save(const std::string &path) const
{
    try
    {
        // Other parts of the game keep their own keys in this file (e.g. "environment"): leave them alone
        nlohmann::json j = JsonFile::ReadObjectFromFileName(path);
        j["windowMode"] = static_cast<int>(windowMode);
        j["windowWidth"] = windowW;
        j["windowHeight"] = windowH;
        j["fullscreenWidth"] = fullscreenW;
        j["fullscreenHeight"] = fullscreenH;
        j["vsync"] = vsync;
        j["fpsLimit"] = fpsLimit;
        j["enabledMods"] = enabledMods;
        j["showFps"] = showFps;
        j["showNetStats"] = showNetStats;
        j["graphicsQuality"] = graphicsQuality;
        j["smoothing"] = smoothing;
        j["showShadows"] = showShadows;
        j["zoomDefault"] = zoomDefault;
        j["zoomMin"] = zoomMin;
        j["zoomMax"] = zoomMax;
        j["colorGrade"] = colorGrade;
        j["glow"] = glow;
        j["sharpPixels"] = sharpPixels;
        j["generalVolume"] = generalVolume;
        j["musicVolume"] = musicVolume;
        j["fxVolume"] = fxVolume;
        j["reverb"] = reverb;
        j["voiceVolume"] = voiceVolume;
        j["ambientVolume"] = ambientVolume;
        j["uiVolume"] = uiVolume;
        j["muted"] = muted;
        j["language"] = language;
        j["playerName"] = playerName;
        j["lastAddress"] = lastAddress;
        j["difficulty"] = difficulty;
        j["edgePanMargin"] = edgePanMargin;
        j["showBuildGrid"] = showBuildGrid;
        j["cameraShake"] = cameraShake;

        return JsonFile::WriteAtomic(path, j);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Settings] Save failed: " << e.what() << std::endl;
        return false;
    }
}

bool Settings::Load(const std::string &path)
{
    try
    {
        std::ifstream fh(path);
        if (!fh.is_open())
            return false;
        const nlohmann::json j = nlohmann::json::parse(fh, nullptr, false);
        if (!j.is_object())
            return false;
        ApplyJson(j);
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Settings] Load failed: " << e.what() << std::endl;
        return false;
    }
}

void Settings::ApplyFactoryDefaults(const nlohmann::json &section)
{
    if (!section.is_object())
        return;
    try
    {
        ApplyJson(section);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Settings] Factory defaults ignored: " << e.what() << std::endl;
    }
}

void Settings::ApplyJson(const nlohmann::json &j)
{
    windowMode = static_cast<WindowMode>(j.value("windowMode", static_cast<int>(windowMode)));
    windowW = j.value("windowWidth", windowW);
    windowH = j.value("windowHeight", windowH);
    fullscreenW = j.value("fullscreenWidth", fullscreenW);
    fullscreenH = j.value("fullscreenHeight", fullscreenH);
    vsync = j.value("vsync", vsync);
#if OAOW_TOUCH
    vsync = true; // without it the mobile builds glitch: not up to a stored choice of an older build
#endif
    fpsLimit = j.value("fpsLimit", fpsLimit);
    enabledMods.clear();
    if (j.contains("enabledMods") && j["enabledMods"].is_array())
        for (const auto &m : j["enabledMods"])
            if (m.is_string())
                enabledMods.push_back(m.get<std::string>());
    showFps = j.value("showFps", showFps);
    showNetStats = j.value("showNetStats", showNetStats);
    graphicsQuality = j.value("graphicsQuality", graphicsQuality);
    smoothing = j.value("smoothing", smoothing);
    showShadows = j.value("showShadows", showShadows);
    zoomDefault = j.value("zoomDefault", zoomDefault);
    zoomMin = j.value("zoomMin", zoomMin);
    zoomMax = j.value("zoomMax", zoomMax);
    colorGrade = j.value("colorGrade", colorGrade);
    glow = j.value("glow", glow);
    sharpPixels = j.value("sharpPixels", sharpPixels);
    generalVolume = j.value("generalVolume", generalVolume);
    musicVolume = j.value("musicVolume", musicVolume);
    fxVolume = j.value("fxVolume", fxVolume);
    reverb = j.value("reverb", reverb);
    voiceVolume = j.value("voiceVolume", voiceVolume);
    ambientVolume = j.value("ambientVolume", ambientVolume);
    uiVolume = j.value("uiVolume", uiVolume);
    muted = j.value("muted", muted);
    language = j.value("language", language);
    playerName = j.value("playerName", playerName);
    lastAddress = j.value("lastAddress", lastAddress);
    difficulty = j.value("difficulty", difficulty);
    edgePanMargin = j.value("edgePanMargin", edgePanMargin);
    showBuildGrid = j.value("showBuildGrid", showBuildGrid);
    cameraShake = j.value("cameraShake", cameraShake);
    Clamp();
}

} // namespace OpenArtOfWar
