cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(lumen_photo VERSION 0.1.0 LANGUAGES CXX)

# The Dusklight version to build against: a release tag or a commit SHA.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.2")
endif ()
# Fetches the Dusklight sources into ./dusklight and provides DUSKLIGHT_DIR.
# Use -DDUSKLIGHT_DIR= to point at an existing Dusklight checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(lumen_photo
        FEATURES fmt game webgpu
        SOURCES
            src/mod.cpp
            ../../../stitcher/src/stitch.cpp
            ../../../stitcher/src/bigtiff.cpp
            ../../../core/src/json.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
)
# The stitcher is compiled in so completed jobs auto-stitch in the background.
# stitch.h includes "tilemath.h" bare, and stitch.cpp includes the json + stb
# headers the same way the standalone stitcher build resolves them.
target_include_directories(lumen_photo PRIVATE third_party "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../stitcher/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../stitcher/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/src")
