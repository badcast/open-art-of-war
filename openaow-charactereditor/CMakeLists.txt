set(CHARACTER_EDITOR_SOURCES
    src/main.cpp
    src/CommentedYaml.cpp
    src/ConfigMerge.cpp
    src/EditorEffects.cpp
    src/EditorProjectiles.cpp
    src/EditorSettings.cpp
    src/EditorGameUi.cpp
    src/EditorEnvironment.cpp
    src/EditorAi.cpp
    src/EditorAnimator.cpp
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

# Saving configs without losing unknown fields (ConfigMerge, CommentedYaml)
add_executable(openaow-charactereditor-tests tests/ConfigMergeTests.cpp src/ConfigMerge.cpp src/CommentedYaml.cpp)
target_include_directories(openaow-charactereditor-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openaow-charactereditor-tests PRIVATE openaow-core)
set_target_properties(openaow-charactereditor-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME openaow-charactereditor-tests COMMAND openaow-charactereditor-tests)
