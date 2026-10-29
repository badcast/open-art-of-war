set(EDITOR_SOURCES
    src/main.cpp
    src/MapEditorApp.cpp
    src/MapDocument.cpp
    src/MapAutoGenerator.cpp
    src/SmartGenerator.cpp
    src/EditorUI.cpp
)

openaow_executable(openaow-mapeditor openaow-mapeditor ${EDITOR_SOURCES})
target_include_directories(openaow-mapeditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
