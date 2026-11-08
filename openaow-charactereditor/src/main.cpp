#include "CharacterEditor.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#endif

static const char *const kHelp = R"(Usage: openaow-charactereditor

Editor of the units and buildings in assets/characters/*.yaml (and of effects, projectiles, the game UI,
the environment, AI difficulty and sprites). Opens the editor window.

  --help, -h    this text
)";

int main(int argc, char **argv)
{
#if defined(_WIN32)
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == NULL || hOut == INVALID_HANDLE_VALUE || GetFileType(hOut) == FILE_TYPE_UNKNOWN)
        {
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
        }
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr == NULL || hErr == INVALID_HANDLE_VALUE || GetFileType(hErr) == FILE_TYPE_UNKNOWN)
        {
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
    }
#endif
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))
    {
        std::cout << kHelp;
        return 0;
    }
    CharEditor::CharacterEditor editor;
    if (!editor.Init())
    {
        std::cerr << editor.ErrorMessage() << '\n';
        return 1;
    }
    return editor.run();
}
