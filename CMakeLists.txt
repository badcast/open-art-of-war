cmake_minimum_required(VERSION 3.16)
# VERSION holds "<major>.<minor>.<patch>[-<stage>]", e.g. 1.0.0-alpha
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION OAOW_VERSION_FULL LIMIT_COUNT 1)
string(STRIP "${OAOW_VERSION_FULL}" OAOW_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" OAOW_VERSION_NUMERIC "${OAOW_VERSION_FULL}")

project(OpenArtOfWar VERSION ${OAOW_VERSION_NUMERIC} LANGUAGES CXX C)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug build: cheats, the debug overlay, --debug and the OAOW_* environment variables.
# The macro AOW_DEBUG exists only when this is ON (code checks it with #if defined(AOW_DEBUG)).
option(AOW_DEBUG "Build the game in debug mode (enables cheats, --debug, OAOW_* variables)" OFF)
# A string, not an option(): option() is a boolean and turned the name into "OFF"
if(DEFINED CACHE{MAIN_ASSET_BLOB_NAME} AND (MAIN_ASSET_BLOB_NAME STREQUAL "OFF" OR MAIN_ASSET_BLOB_NAME STREQUAL "ON"))
    unset(MAIN_ASSET_BLOB_NAME CACHE)
endif()
set(MAIN_ASSET_BLOB_NAME "assets.blob" CACHE STRING "The name of the blob file that holds the packed contents of the \"assets\" directory. The blob is a tar stream compressed.")
option(BUILD_ASSETS_BLOB "Make a ${MAIN_ASSET_BLOB_NAME} with: assets" OFF)

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
    src/core/ApplicationSettings.cpp
    src/core/ApplicationNetwork.cpp
    src/core/ApplicationOverlays.cpp
    src/core/Localization.cpp
    src/core/AssetStore.cpp
    src/core/Mods.cpp
    src/core/Sha256.cpp
    src/core/LoaderResources.cpp
    src/audio/SfxMixer.cpp
    src/audio/GameSounds.cpp
    src/audio/SoundIds.cpp
    src/audio/VoiceManager.cpp
    src/core/Settings.cpp
    src/core/VideoModes.cpp
    src/core/EnvironmentConfig.cpp
    src/core/JsonFile.cpp
    src/graphics/Camera.cpp
    src/graphics/TextureManager.cpp
    src/graphics/SpriteRegistry.cpp
    src/graphics/Renderer.cpp
    src/graphics/RenderUtility.cpp
    src/graphics/RenderBackend.cpp
    src/graphics/ShaderEffects.cpp
    src/graphics/NightLighting.cpp
    src/graphics/BitmapFontRenderer.cpp
    src/input/InputManager.cpp
    src/audio/SoundEngine.cpp
    src/game/TileMap.cpp
    src/game/TileCollision.cpp
    src/game/Pathfinder.cpp
    src/game/Entity.cpp
    src/game/Unit.cpp
    src/game/UnitMovement.cpp
    src/game/UnitRightOfWay.cpp
    src/game/UnitCombat.cpp
    src/game/Building.cpp
    src/game/EntityManager.cpp
    src/game/EntityManagerHelpers.cpp
    src/game/EntityManagerConfig.cpp
    src/game/EntityManagerSpawn.cpp
    src/game/EntityManagerOrders.cpp
    src/game/EntityManagerSiege.cpp
    src/game/EntityManagerFog.cpp
    src/game/EntityManagerUpdate.cpp
    src/game/EntityManagerProjectiles.cpp
    src/game/EntityManagerDeath.cpp
    src/game/EntityManagerRender.cpp
    src/game/Economy.cpp
    src/game/AIController.cpp
    src/game/AIControllerHelpers.cpp
    src/game/AIControllerWaves.cpp
    src/game/AIControllerEconomy.cpp
    src/game/AIControllerDefense.cpp
    src/game/AIControllerIntel.cpp
    src/game/AIControllerRebuild.cpp
    src/game/AIBaseLayout.cpp
    src/game/AISquadController.cpp
    src/game/Research.cpp
    src/game/Effects.cpp
    src/game/Projectiles.cpp
    src/game/WeaponBalance.cpp
    src/game/BuildLimits.cpp
    src/game/Sides.cpp
    src/game/Players.cpp
    src/core/Utility.cpp
    src/game/MissionController.cpp
    src/ui/MapPreview.cpp
    src/ui/MenuBackdropFx.cpp
    src/game/Missions.cpp
    src/game/MissionDialogs.cpp
    src/game/MissionsResistance.cpp
    src/game/MissionsConfederation.cpp
    src/game/GameplayController.cpp
    src/game/Simulation.cpp
    src/game/SimCommandCodec.cpp
    src/net/NetMatch.cpp
    src/net/MapFiles.cpp
    src/net/ReplayRecorder.cpp
    src/replay/ReplayPlayer.cpp
    src/ui/ReplayMenu.cpp
    src/ui/ModsMenu.cpp
    src/core/ApplicationReplay.cpp
    src/ui/MenuController.cpp
    src/ui/MenuControllerInGame.cpp
    src/ui/MenuControllerSettings.cpp
    src/ui/MenuControllerCampaign.cpp
    src/ui/MenuControllerCustom.cpp
    src/ui/HUDController.cpp
    src/ui/HUDControllerRender.cpp
    src/ui/HUDControllerInput.cpp
    src/ui/HUDControllerOverlays.cpp
    src/ui/ResearchWindow.cpp
    src/ui/UiKit.cpp
    src/ui/ImGuiBridge.cpp
    src/ui/NetworkMenu.cpp
    src/storage/SaveManager.cpp
    src/debug/DebugOverlay.cpp
)

# Translations (gettext MO generation)
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES)
    add_custom_target(translations ALL
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES/openaow.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_en.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES/openaow.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_ru.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES/openaow_character_editor.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_character_editor_en.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES/openaow_character_editor.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_character_editor_ru.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/en/LC_MESSAGES/openaow_map_editor.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_map_editor_en.po
        COMMAND ${MSGFMT_EXECUTABLE} -o ${CMAKE_CURRENT_SOURCE_DIR}/assets/locale/ru/LC_MESSAGES/openaow_map_editor.mo ${CMAKE_CURRENT_SOURCE_DIR}/po/openaow_map_editor_ru.po
        COMMENT "Compiling gettext translation catalogs (.po -> .mo)..."
    )
endif()

enable_testing()

# Network layer: OpenAOWNetworking static library (ENet inside), linked into the engine and the server
add_subdirectory(src/networking)

# Dedicated server: the network library only (no SDL, no game code)
option(OAOW_BUILD_SERVER "Build openaow-server" ON)
if(OAOW_BUILD_SERVER)
    add_executable(openaow-server openaow-server/main.cpp)
    target_link_libraries(openaow-server PRIVATE OpenAOWNetworking nlohmann_json::nlohmann_json)
    target_compile_definitions(openaow-server PRIVATE $<$<BOOL:${AOW_DEBUG}>:AOW_DEBUG> OAOW_VERSION="${OAOW_VERSION_FULL}")
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
    if(MINGW)
        # Do not re-export the static libgcc/libstdc++ (_Unwind_Resume) from the DLL: it clashes with the exe
        target_link_options(openaow-core PRIVATE -Wl,--exclude-libs,ALL)
    endif()
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
target_compile_definitions(openaow-core PUBLIC
            $<$<BOOL:${AOW_DEBUG}>:AOW_DEBUG>
            MAIN_ASSET_BLOB_NAME="${MAIN_ASSET_BLOB_NAME}"
            OAOW_VERSION="${OAOW_VERSION_FULL}"
            $<$<CONFIG:Debug>:OAOW_DEBUG>
)
if(TARGET translations)
    add_dependencies(openaow-core translations)
endif()

# The contents of assets/ packed into one tar.zst next to the executables. The game looks
# for the assets directory first and falls back to this file (see core/AssetStore.hpp).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND BUILD_ASSETS_BLOB)
    file(GLOB_RECURSE OAOW_ASSET_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/*)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/${MAIN_ASSET_BLOB_NAME}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_assets.py
                ${CMAKE_CURRENT_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/${MAIN_ASSET_BLOB_NAME}
        DEPENDS ${OAOW_ASSET_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_assets.py
        COMMENT "Packing assets and their certificate into ${MAIN_ASSET_BLOB_NAME}..."
    )
    add_custom_target(pack-assets ALL DEPENDS ${CMAKE_BINARY_DIR}/${MAIN_ASSET_BLOB_NAME})
    if(TARGET translations)
        add_dependencies(pack-assets translations)
    endif()
else()
    message(WARNING "Python3 not found: ${MAIN_ASSET_BLOB_NAME} will not be built (the game still runs from the assets directory)")
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

    if(WIN32)
        # Windows Desktop manifest, icon and version resource
        if(target STREQUAL "openaow-game")
            set(OAOW_MANIFEST_NAME "OpenArtOfWar.Game")
            set(OAOW_MANIFEST_IN "manifest-game.in")
            set(OAOW_RC_IN "manifest-game.rc.in")
            set(OAOW_ICON_PATH "${PROJECT_SOURCE_DIR}/docs/app-game-icon.ico")
            set(OAOW_DESCRIPTION "Open Art Of War: Freedom for Liberty")
        elseif(target STREQUAL "openaow-charactereditor")
            set(OAOW_MANIFEST_NAME "OpenArtOfWar.CharacterEditor")
            set(OAOW_MANIFEST_IN "manifest-charactereditor.in")
            set(OAOW_RC_IN "manifest-charactereditor.rc.in")
            set(OAOW_ICON_PATH "${PROJECT_SOURCE_DIR}/docs/app-chareditor-icon.ico")
            set(OAOW_DESCRIPTION "Open Art Of War Character Archetype Editor")
        elseif(target STREQUAL "openaow-mapeditor")
            set(OAOW_MANIFEST_NAME "OpenArtOfWar.MapEditor")
            set(OAOW_MANIFEST_IN "manifest-mapeditor.in")
            set(OAOW_RC_IN "manifest-mapeditor.rc.in")
            set(OAOW_ICON_PATH "${PROJECT_SOURCE_DIR}/docs/app-mapeditor-icon.ico")
            set(OAOW_DESCRIPTION "Open Art Of War Map Editor")
        endif()

        if(OAOW_MANIFEST_IN)
            set(OAOW_TARGET_NAME "${output}")
            set(OAOW_ORIGINAL_FILENAME "${output}.exe")
            set(OAOW_MANIFEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/${output}.manifest")
            set(OAOW_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${output}.rc")

            configure_file(
                "${PROJECT_SOURCE_DIR}/config/${OAOW_MANIFEST_IN}"
                "${OAOW_MANIFEST_FILE}"
                @ONLY
            )
            configure_file(
                "${PROJECT_SOURCE_DIR}/config/${OAOW_RC_IN}"
                "${OAOW_RC_FILE}"
                @ONLY
            )
            target_sources(${target} PRIVATE "${OAOW_RC_FILE}")
            if(MSVC)
                # The .rc already embeds the manifest: the linker's own one would be a second RT_MANIFEST (CVT1100)
                target_link_options(${target} PRIVATE /MANIFEST:NO)
            endif()
        endif()

        # In Release mode, hide console window for desktop GUI applications
        set_property(TARGET ${target} PROPERTY WIN32_EXECUTABLE $<CONFIG:Release>)
        if(MSVC)
            # Link standard main(int, char**) as the entry point for GUI subsystem
            target_link_options(${target} PRIVATE $<$<BOOL:$<TARGET_PROPERTY:WIN32_EXECUTABLE>>:/ENTRY:mainCRTStartup>)
        endif()

        # Windows has no rpath: put the DLLs of the dependencies next to the executable
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS)
        endif()
    endif()
endfunction()

# Game
openaow_executable(openaow-game openaow src/main.cpp)

# Standalone YAML character archetype editor using the game's renderer and assets
add_subdirectory(openaow-charactereditor charactereditor_build)

# Map editor
add_subdirectory(openaow-mapeditor mapeditor_build)

# Engine unit tests (no window, no assets)
add_executable(openaow-sprite-tests tests/SpriteRegistryTests.cpp)
target_link_libraries(openaow-sprite-tests PRIVATE openaow-core)
set_target_properties(openaow-sprite-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${OAOW_RPATH}")
add_test(NAME openaow-sprite-tests COMMAND openaow-sprite-tests)

add_executable(openaow-ai-layout-tests tests/AIBaseLayoutTests.cpp)
target_link_libraries(openaow-ai-layout-tests PRIVATE openaow-core)
set_target_properties(openaow-ai-layout-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${OAOW_RPATH}")
add_test(NAME openaow-ai-layout-tests COMMAND openaow-ai-layout-tests)

add_executable(openaow-build-limits-tests tests/BuildLimitsTests.cpp)
target_link_libraries(openaow-build-limits-tests PRIVATE openaow-core)
set_target_properties(openaow-build-limits-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${OAOW_RPATH}")
add_test(NAME openaow-build-limits-tests COMMAND openaow-build-limits-tests)

add_executable(openaow-mods-tests tests/ModsTests.cpp)
target_link_libraries(openaow-mods-tests PRIVATE openaow-core)
set_target_properties(openaow-mods-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "${OAOW_RPATH}")
add_test(NAME openaow-mods-tests COMMAND openaow-mods-tests)
