# Dependencies of the iOS build: built statically from source for the selected iOS SDK (iphoneos / iphonesimulator).
# Provides the targets the main CMakeLists.txt expects:
#   SDL3::SDL3, SDL3_ttf::SDL3_ttf, nlohmann_json::nlohmann_json, yaml-cpp::yaml-cpp, PkgConfig::LIBARCHIVE
# and the variables YAML_CPP_LIBRARIES, YAML_CPP_INCLUDE_DIRS, OAOW_ZSTD.
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(FETCHCONTENT_QUIET OFF)

# Force static libraries across all fetched dependencies on iOS
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# nlohmann_json (header only)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW ON)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

# yaml-cpp (static)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git GIT_TAG 0.8.0 GIT_SHALLOW ON)
set(YAML_CPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)

# zstd (static): built and installed upfront so libarchive finds it during configure
set(OAOW_ZSTD_SRC "${CMAKE_BINARY_DIR}/zstd-src")
set(OAOW_ZSTD_PREFIX "${CMAKE_BINARY_DIR}/zstd-install")
if(NOT EXISTS "${OAOW_ZSTD_PREFIX}/lib/libzstd.a")
    if(NOT EXISTS "${OAOW_ZSTD_SRC}/build/cmake/CMakeLists.txt")
        execute_process(COMMAND git clone --quiet --depth 1 --branch v1.5.6 https://github.com/facebook/zstd.git "${OAOW_ZSTD_SRC}"
            RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "Cannot clone zstd")
        endif()
    endif()
    set(zstd_args
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_INSTALL_PREFIX=${OAOW_ZSTD_PREFIX}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO
        -DZSTD_BUILD_PROGRAMS=OFF
        -DZSTD_BUILD_TESTS=OFF
        -DZSTD_BUILD_CONTRIB=OFF
        -DZSTD_BUILD_SHARED=OFF
        -DZSTD_BUILD_STATIC=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
    foreach(var CMAKE_OSX_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_TOOLCHAIN_FILE)
        if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
            list(APPEND zstd_args -D${var}=${${var}})
        endif()
    endforeach()
    set(zstd_gen "${CMAKE_GENERATOR}")
    set(zstd_build "${OAOW_ZSTD_SRC}/_b")
    foreach(step "configure" "build" "install")
        if(step STREQUAL "configure")
            set(cmd ${CMAKE_COMMAND} -G "${zstd_gen}" -S "${OAOW_ZSTD_SRC}/build/cmake" -B "${zstd_build}" ${zstd_args})
        elseif(step STREQUAL "build")
            set(cmd ${CMAKE_COMMAND} --build "${zstd_build}" --config Release)
        else()
            set(cmd ${CMAKE_COMMAND} --install "${zstd_build}" --config Release)
        endif()
        execute_process(COMMAND ${cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "zstd ${step} failed:\n${out}")
        endif()
    endforeach()
endif()
add_library(oaow-zstd STATIC IMPORTED GLOBAL)
set_target_properties(oaow-zstd PROPERTIES
    IMPORTED_LOCATION "${OAOW_ZSTD_PREFIX}/lib/libzstd.a"
    INTERFACE_INCLUDE_DIRECTORIES "${OAOW_ZSTD_PREFIX}/include")

# SDL3 and SDL3_ttf (static libraries on iOS)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.20 GIT_SHALLOW ON)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git GIT_TAG release-3.2.2 GIT_SHALLOW ON)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json yaml-cpp SDL3)
FetchContent_MakeAvailable(SDL3_ttf)

set(YAML_CPP_LIBRARIES yaml-cpp::yaml-cpp)
set(YAML_CPP_INCLUDE_DIRS "")
set(OAOW_ZSTD oaow-zstd)

# libarchive (static tar + zstd for assets.blob and replays)
FetchContent_Declare(libarchive
    GIT_REPOSITORY https://github.com/libarchive/libarchive.git GIT_TAG v3.7.7 GIT_SHALLOW ON)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)
foreach(opt TAR CPIO CAT UNZIP)
    set(ENABLE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt OPENSSL NETTLE MBEDTLS LIBB2 LZ4 LZO LZMA BZip2 ZLIB LIBXML2 EXPAT PCREPOSIX PCRE2POSIX LIBGCC ICONV ACL XATTR CNG)
    set(ENABLE_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_ZSTD ON CACHE BOOL "" FORCE)
set(ZSTD_INCLUDE_DIR "${OAOW_ZSTD_PREFIX}/include" CACHE PATH "" FORCE)
set(ZSTD_LIBRARY "${OAOW_ZSTD_PREFIX}/lib/libzstd.a" CACHE FILEPATH "" FORCE)
FetchContent_MakeAvailable(libarchive)
add_library(PkgConfig::LIBARCHIVE INTERFACE IMPORTED GLOBAL)
target_link_libraries(PkgConfig::LIBARCHIVE INTERFACE archive_static)
target_include_directories(PkgConfig::LIBARCHIVE INTERFACE "${libarchive_SOURCE_DIR}/libarchive")
