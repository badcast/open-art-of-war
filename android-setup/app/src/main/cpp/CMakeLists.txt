cmake_minimum_required(VERSION 3.22.1)
project(openaow_android LANGUAGES CXX C)

# Only the game itself: no server, editors or tests
set(OAOW_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(OAOW_BUILD_NET_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ASSETS_BLOB OFF CACHE BOOL "" FORCE)

# The repository root builds the engine, the networking library and libmain.so (see its ANDROID branch)
get_filename_component(OAOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_subdirectory(${OAOW_ROOT} openaow)

# Android system libraries for the game
target_link_libraries(main PRIVATE android log)
