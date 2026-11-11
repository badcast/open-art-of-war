cmake_minimum_required(VERSION 3.22.1)
project(openaow_android LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OAOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../")

# Flags
add_compile_options(-Wall -Wextra -O2 -ffp-contract=off)
add_compile_definitions(ANDROID)

# Third-party / Fetch dependencies when compiling for Android NDK
include(FetchContent)

# 1. nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# 2. yaml-cpp
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# 3. SDL3 and SDL3_ttf (NDK subprojects or FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.0
)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.0
)
set(SDL3TTF_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_ttf)

# Includes
include_directories(
    ${OAOW_ROOT}/include
    ${OAOW_ROOT}/3rdParty/imgui
    ${OAOW_ROOT}/3rdParty/imgui/backends
    ${OAOW_ROOT}/3rdParty/library-brainmap
    ${OAOW_ROOT}/3rdParty/stb
    ${OAOW_ROOT}/3rdParty/enet/include
    ${OAOW_ROOT}/3rdParty/ronin-engine/include
)

# ImGui sources
set(IMGUI_SOURCES
    ${OAOW_ROOT}/3rdParty/imgui/imgui.cpp
    ${OAOW_ROOT}/3rdParty/imgui/imgui_draw.cpp
    ${OAOW_ROOT}/3rdParty/imgui/imgui_tables.cpp
    ${OAOW_ROOT}/3rdParty/imgui/imgui_widgets.cpp
    ${OAOW_ROOT}/3rdParty/imgui/backends/imgui_impl_sdl3.cpp
    ${OAOW_ROOT}/3rdParty/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

# Engine sources
file(GLOB_RECURSE OAOW_SRC
    "${OAOW_ROOT}/src/audio/*.cpp"
    "${OAOW_ROOT}/src/core/*.cpp"
    "${OAOW_ROOT}/src/game/*.cpp"
    "${OAOW_ROOT}/src/graphics/*.cpp"
    "${OAOW_ROOT}/src/input/*.cpp"
    "${OAOW_ROOT}/src/networking/*.cpp"
    "${OAOW_ROOT}/src/ui/*.cpp"
)

# Filter out server-only or test files if present
list(FILTER OAOW_SRC EXCLUDE REGEX ".*/tests/.*")
list(FILTER OAOW_SRC EXCLUDE REGEX ".*/server/.*")

# Third-party C sources
set(STB_VORBIS_SOURCE ${OAOW_ROOT}/3rdParty/stb/stb_vorbis.c)
set(BRAINMAP_SOURCES ${OAOW_ROOT}/3rdParty/library-brainmap/Brainmap.cpp)

file(GLOB ENET_SOURCES "${OAOW_ROOT}/3rdParty/enet/*.c")
file(GLOB RONIN_SOURCES "${OAOW_ROOT}/3rdParty/ronin-engine/src/*.cpp")

# Main Android library (must be SHARED for SDLActivity to load)
# SDLActivity loads libmain.so by default
add_library(main SHARED
    ${OAOW_ROOT}/src/main.cpp
    ${OAOW_SRC}
    ${IMGUI_SOURCES}
    ${BRAINMAP_SOURCES}
    ${ENET_SOURCES}
    ${RONIN_SOURCES}
    ${STB_VORBIS_SOURCE}
)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    nlohmann_json::nlohmann_json
    yaml-cpp::yaml-cpp
    android
    log
    GLESv3
    OpenSLES
)
