set(CHARACTER_EDITOR_SOURCES
    src/main.cpp
    src/CommentedYaml.cpp
    src/EditorEffects.cpp
    src/EditorSettings.cpp
    src/EditorGameUi.cpp
    src/EditorEnvironment.cpp
    src/EditorAi.cpp
    src/BattleSim.cpp
    src/CharacterEditor.cpp
    src/EditorCharacterDraw.cpp
    src/EditorCompare.cpp
    src/EditorHints.cpp
    src/EditorPreview.cpp
    src/EditorResearch.cpp
    src/EditorSprites.cpp
    src/SpriteCatalog.cpp
    src/SpriteExport.cpp
    src/EditorProperties.cpp
    src/EditorUtils.cpp
)

openaow_executable(openaow-charactereditor openaow-charactereditor ${CHARACTER_EDITOR_SOURCES})
target_include_directories(openaow-charactereditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
