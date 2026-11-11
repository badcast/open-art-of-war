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
    src/EditorSpriteMerge.cpp
    src/SpriteMerge.cpp
    src/EditorProperties.cpp
    src/EditorUtils.cpp
)

openaow_executable(openaow-charactereditor openaow-charactereditor ${CHARACTER_EDITOR_SOURCES})
target_include_directories(openaow-charactereditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Sprite Merge looks for sprite names the game code uses (they cannot be rewritten)
target_compile_definitions(openaow-charactereditor PRIVATE OPENAOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Saving configs without losing unknown fields (ConfigMerge, CommentedYaml)
add_executable(openaow-charactereditor-tests tests/ConfigMergeTests.cpp src/ConfigMerge.cpp src/CommentedYaml.cpp)
target_include_directories(openaow-charactereditor-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openaow-charactereditor-tests PRIVATE openaow-core)
set_target_properties(openaow-charactereditor-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OAOW_TESTS_DIR}"
    BUILD_RPATH "${OAOW_TESTS_RPATH}")
add_test(NAME openaow-charactereditor-tests COMMAND openaow-charactereditor-tests)

# Merging sprite objects (SpriteMerge)
add_executable(openaow-spritemerge-tests tests/SpriteMergeTests.cpp src/SpriteMerge.cpp)
target_include_directories(openaow-spritemerge-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openaow-spritemerge-tests PRIVATE openaow-core)
set_target_properties(openaow-spritemerge-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OAOW_TESTS_DIR}"
    BUILD_RPATH "${OAOW_TESTS_RPATH}")
add_test(NAME openaow-spritemerge-tests COMMAND openaow-spritemerge-tests)

add_executable(openaow-preview-tests tests/PreviewStatsTests.cpp src/EditorUtils.cpp)
target_include_directories(openaow-preview-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openaow-preview-tests PRIVATE openaow-core)
set_target_properties(openaow-preview-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OAOW_TESTS_DIR}"
    BUILD_RPATH "${OAOW_TESTS_RPATH}")
add_test(NAME openaow-preview-tests COMMAND openaow-preview-tests)
