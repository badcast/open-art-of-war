cmake_minimum_required(VERSION 3.16)
# VERSION holds "<major>.<minor>.<patch>[-<stage>]", e.g. 1.0.0-alpha
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION OAOW_VERSION_FULL LIMIT_COUNT 1)
string(STRIP "${OAOW_VERSION_FULL}" OAOW_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" OAOW_VERSION_NUMERIC "${OAOW_VERSION_FULL}")

project(OpenArtOfWar VERSION ${OAOW_VERSION_NUMERIC} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_ASSETS_BLOB "Make a data.blob with: assets" OFF)
option(AOW_DEBUG "Build the game in debug mode (enables cheats)" OFF)

# Optimization and warning flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
    # Lockstep: a*b+c must round the same on every CPU, so no fused multiply-add (ARM, x86 with FMA)
    add_compile_options(-ffp-contract=off)
elseif(MSVC)
    # Sources are UTF-8 (Cyrillic UI strings); M_PI and std::min/max need these on Windows
    add_compile_options(/utf-8 /W3 /permissive-)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()
if(APPLE)
    set(OAOW_RPATH "@loader_path")
else()
    set(OAOW_RPATH "$ORIGIN")
endif()

# Find dependencies
find_package(PkgConfig QUIET)
# SDL3 core (window, renderer, input, audio streams, PNG loading) + SDL3_ttf for text
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Intl REQUIRED)  # gettext runtime (part of libc on glibc)
find_package(yaml-cpp CONFIG QUIET)
if(TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_LIBRARIES yaml-cpp::yaml-cpp)
    set(YAML_CPP_INCLUDE_DIRS "")
else()
    pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)
endif()
# libarchive: reads data.blob (tar + zstd)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
# zstd: maps sent between players in a network game are compressed with it (libarchive needs it anyway)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
    set(OAOW_ZSTD zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    set(OAOW_ZSTD zstd::libzstd_static)
else()
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(OAOW_ZSTD PkgConfig::ZSTD)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/library-brainmap
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/stb
    ${YAML_CPP_INCLUDE_DIRS}
)

# Ogg Vorbis decoder for music (third-party C code, built without project warnings)
set(STB_VORBIS_SOURCE 3rdParty/stb/stb_vorbis.c)
set_source_files_properties(${STB_VORBIS_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")


# ImGui sources
set(IMGUI_SOURCES
    3rdParty/imgui/imgui.cpp
    3rdParty/imgui/imgui_draw.cpp
    3rdParty/imgui/imgui_tables.cpp
    3rdParty/imgui/imgui_widgets.cpp
    3rdParty/imgui/backends/imgui_impl_sdl3.cpp
    3rdParty/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

# Engine sources (everything except the executables' own main files)
set(ENGINE_SOURCES
    src/core/Application.cpp
    src/core/Localization.cpp
    src/core/AssetStore.cpp
    src/core/Settings.cpp
    src/core/VideoModes.cpp
    src/core/EnvironmentConfig.cpp
    src/core/JsonFile.cpp
    src/graphics/Camera.cpp
    src/graphics/TextureManager.cpp
    src/graphics/Renderer.cpp
    src/graphics/RenderUtility.cpp
    src/graphics/RenderBackend.cpp
    src/graphics/ShaderEffects.cpp
    src/graphics/BitmapFontRenderer.cpp
    src/input/InputManager.cpp
    src/audio/SoundEngine.cpp
    src/game/TileMap.cpp
    src/game/TileCollision.cpp
    src/game/Pathfinder.cpp
    src/game/Entity.cpp
    src/game/Unit.cpp
    src/game/Building.cpp
    src/game/EntityManager.cpp
    src/game/Economy.cpp
    src/game/AIController.cpp
    src/game/AISquadController.cpp
    src/game/Research.cpp
    src/game/Effects.cpp
    src/game/Sides.cpp
    src/game/Players.cpp
    src/core/Utility.cpp
    src/game/MissionController.cpp
    src/ui/MapPreview.cpp
    src/ui/MenuBackdropFx.cpp
    src/game/Missions.cpp
    src/game/MissionsResistance.cpp
    src/game/MissionsConfederation.cpp
    src/game/GameplayController.cpp
    src/game/Simulation.cpp
    src/game/SimCommandCodec.cpp
    src/net/NetMatch.cpp
    src/net/MapFiles.cpp
    src/ui/MenuController.cpp
    src/ui/HUDController.cpp
    src/ui/UiKit.cpp
    src/ui/NetworkMenu.cpp
    src/test/HeadlessTest.cpp
    src/storage/SaveManager.cpp
    src/debug/DebugOverlay.cpp
)

# Translations (gettext MO generation)
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES)
    add_custom_target(translations ALL
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES/openaow.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/en.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES/openaow.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/ru.po
        COMMENT "Compiling gettext translation catalogs (.po -> .mo)..."
    )
endif()

# Network layer: OpenAOWNetworking static library (ENet inside), linked into the engine and the server
enable_testing()
add_subdirectory(networking)

# Dedicated server: the network library only (no SDL, no game code)
option(OAOW_BUILD_SERVER "Build openaow-server" ON)
if(OAOW_BUILD_SERVER)
    add_executable(openaow-server openaow-server/main.cpp)
    target_link_libraries(openaow-server PRIVATE OpenAOWNetworking nlohmann_json::nlohmann_json)
    target_compile_definitions(openaow-server PRIVATE AOW_DEBUG=${AOW_DEBUG} OAOW_VERSION="${OAOW_VERSION_FULL}")
    set_target_properties(openaow-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# Shader effects of the SDL GPU renderer (shaders/*.frag, GLSL): SPIR-V by glslc, Metal source by
# spirv-cross when it is installed; without glslc the precompiled SPIR-V of shaders/compiled is used.
# Everything ends up as byte arrays inside the engine (no shader files at run time).
set(OAOW_SHADER_NAMES sprite post glow blur)
set(OAOW_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${OAOW_SHADER_OUT})
find_program(OAOW_GLSLC glslc)
find_program(OAOW_SPIRV_CROSS spirv-cross)
set(OAOW_SHADER_FILES "")
foreach(name IN LISTS OAOW_SHADER_NAMES)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.frag)
    set(spv ${OAOW_SHADER_OUT}/${name}.frag.spv)
    if(OAOW_GLSLC)
        add_custom_command(OUTPUT ${spv}
            COMMAND ${OAOW_GLSLC} -O -fshader-stage=frag -o ${spv} ${src}
            DEPENDS ${src} COMMENT "Compiling shader ${name}.frag")
    else()
        add_custom_command(OUTPUT ${spv}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compiled/${name}.frag.spv ${spv}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compiled/${name}.frag.spv)
    endif()
    list(APPEND OAOW_SHADER_FILES ${spv})
    if(OAOW_SPIRV_CROSS)
        set(msl ${OAOW_SHADER_OUT}/${name}.frag.msl)
        add_custom_command(OUTPUT ${msl}
            COMMAND ${OAOW_SPIRV_CROSS} --msl --msl-version 20000 --output ${msl} ${spv}
            DEPENDS ${spv} COMMENT "Translating shader ${name} to Metal")
        list(APPEND OAOW_SHADER_FILES ${msl})
    endif()
endforeach()
if(NOT OAOW_GLSLC)
    message(STATUS "glslc not found: using the precompiled SPIR-V of shaders/compiled")
endif()
add_custom_command(OUTPUT ${OAOW_SHADER_OUT}/OaowShaders.inc
    COMMAND ${CMAKE_COMMAND} -DOUT=${OAOW_SHADER_OUT}/OaowShaders.inc -DDIR=${OAOW_SHADER_OUT}
            "-DNAMES=${OAOW_SHADER_NAMES}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${OAOW_SHADER_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    COMMENT "Embedding shaders" VERBATIM)
set_source_files_properties(src/graphics/ShaderEffects.cpp PROPERTIES
    OBJECT_DEPENDS ${OAOW_SHADER_OUT}/OaowShaders.inc)

# Shared engine library: libopenaow.so (engine, game logic, UI, ImGui)
add_library(openaow-core SHARED ${ENGINE_SOURCES} ${IMGUI_SOURCES} ${STB_VORBIS_SOURCE})
set_target_properties(openaow-core PROPERTIES
    OUTPUT_NAME openaow
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"  # the DLL on Windows
    PDB_NAME openaow-core                           # the game's openaow.pdb would clash
    BUILD_RPATH "${OAOW_RPATH}"
    INSTALL_RPATH "${OAOW_RPATH}/../lib"
)
if(WIN32)
    set_target_properties(openaow-core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_include_directories(openaow-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/library-brainmap
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/stb
    ${YAML_CPP_INCLUDE_DIRS}
)
target_link_libraries(openaow-core PUBLIC
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    ${YAML_CPP_LIBRARIES}
    nlohmann_json::nlohmann_json
    Intl::Intl
    PkgConfig::LIBARCHIVE
)
target_link_libraries(openaow-core PRIVATE OpenAOWNetworking ${OAOW_ZSTD})
target_sources(openaow-core PRIVATE ${OAOW_SHADER_OUT}/OaowShaders.inc)
target_include_directories(openaow-core PRIVATE ${OAOW_SHADER_OUT})
target_compile_definitions(openaow-core PUBLIC AOW_DEBUG=${AOW_DEBUG}
    OAOW_VERSION="${OAOW_VERSION_FULL}"
    $<$<CONFIG:Debug>:OAOW_DEBUG>
)
if(TARGET translations)
    add_dependencies(openaow-core translations)
endif()

# data.blob: the contents of assets/ packed into one tar.zst next to the executables. The game looks
# for the assets directory first and falls back to this file (see core/AssetStore.hpp).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND BUILD_ASSETS_BLOB)
    file(GLOB_RECURSE OAOW_ASSET_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/*)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/data.blob
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_assets.py
                ${CMAKE_CURRENT_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/data.blob
        DEPENDS ${OAOW_ASSET_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_assets.py
        COMMENT "Packing assets into data.blob..."
    )
    add_custom_target(pack-assets ALL DEPENDS ${CMAKE_BINARY_DIR}/data.blob)
    if(TARGET translations)
        add_dependencies(pack-assets translations)
    endif()
else()
    message(WARNING "Python3 not found: data.blob will not be built (the game still runs from the assets directory)")
endif()

# Helper: thin executable linked against the shared library
function(openaow_executable target output)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${output}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        BUILD_RPATH "${OAOW_RPATH}"
        INSTALL_RPATH "${OAOW_RPATH}/../lib"
    )
    target_link_libraries(${target} PRIVATE openaow-core)
    # Windows has no rpath: put the DLLs of the dependencies next to the executable
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

# Game
openaow_executable(openaow-game openaow src/main.cpp)

# Standalone YAML character archetype editor using the game's renderer and assets
add_subdirectory(openaow-charactereditor charactereditor_build)

# Map editor
add_subdirectory(openaow-mapeditor mapeditor_build)
