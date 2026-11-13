#include "storage/SaveManager.hpp"
#include "core/AssetStore.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

namespace OpenArtOfWar
{

namespace
{

// Where earlier versions kept the save: next to the game, or below the user directory in blob mode
void MigrateLegacySave(const std::filesystem::path &target)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(target, ec))
        return;
    const fs::path candidates[]
        = {fs::path("saves") / "savegame.json", fs::path(Assets::UserDataDir()) / "saves" / "savegame.json"};
    for (const fs::path &old : candidates)
    {
        if (fs::exists(old, ec) && fs::copy_file(old, target, ec))
        {
            std::cout << "[SaveManager] Moved the save from " << old.string() << " to " << target.string() << std::endl;
            return;
        }
    }
}

} // namespace

SaveManager::SaveManager(std::string_view saveFilePath)
{
    if (!saveFilePath.empty())
    {
        m_savePath.assign(saveFilePath);
        return;
    }
    const std::filesystem::path target = std::filesystem::path(Assets::UserDataDir()) / "savegame.json";
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    MigrateLegacySave(target);
    m_savePath = target.string();
}

bool SaveManager::HasSave() const
{
    return std::filesystem::exists(m_savePath);
}

bool SaveManager::Save(const SaveData &data)
{
    try
    {
        std::filesystem::path p(m_savePath);
        if (p.has_parent_path())
        {
            std::filesystem::create_directories(p.parent_path());
        }

        nlohmann::json j;
        j["unlockedMissions"] = data.unlockedMissions;
        j["currentMission"] = data.currentMission;
        j["credits"] = data.credits;
        j["soundEnabled"] = data.soundEnabled;
        j["language"] = data.language;
        j["difficulty"] = data.difficulty;

        std::ofstream fh(m_savePath);
        if (!fh.is_open())
            return false;
        fh << j.dump(2);
        std::cout << "[SaveManager] Saved progress to " << m_savePath << std::endl;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[SaveManager] Save failed: " << e.what() << std::endl;
        return false;
    }
}

bool SaveManager::Load(SaveData &outData)
{
    if (!HasSave())
        return false;
    try
    {
        std::ifstream fh(m_savePath);
        if (!fh.is_open())
            return false;
        nlohmann::json j;
        fh >> j;

        outData.unlockedMissions = j.value("unlockedMissions", 1);
        outData.currentMission = j.value("currentMission", 0);
        outData.credits = j.value("credits", 1200);
        outData.soundEnabled = j.value("soundEnabled", true);
        outData.language = j.value("language", "en");
        outData.difficulty = j.value("difficulty", 1);

        std::cout << "[SaveManager] Loaded progress from " << m_savePath << std::endl;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[SaveManager] Load failed: " << e.what() << std::endl;
        return false;
    }
}

} // namespace OpenArtOfWar
