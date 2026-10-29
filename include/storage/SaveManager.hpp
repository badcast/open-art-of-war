#pragma once

#include <string>

namespace OpenArtOfWar
{

    struct SaveData
    {
        int unlockedMissions {1};
        int currentMission {0};
        int credits {1200};
        bool soundEnabled {true};
        std::string language {"en"};
        int difficulty {1}; // 0: Easy, 1: Medium, 2: Hard, 3: Hardcore
    };

    // Game progress, stored as savegame.json in the per-user directory (Assets::userDataDir()); the language,
    // difficulty and sound fields are kept for old save files, the options live in Settings now.
    class SaveManager
    {
    public:
        // Empty path: <user directory>/savegame.json. An older ./saves/savegame.json (or the one in the user
        // directory's saves folder) is copied over the first time when the new file does not exist yet.
        explicit SaveManager(const std::string &saveFilePath = "");

        bool save(const SaveData &data);
        bool load(SaveData &outData);
        bool hasSave() const;
        const std::string &path() const
        {
            return m_savePath;
        }

    private:
        std::string m_savePath;
    };

} // namespace OpenArtOfWar
