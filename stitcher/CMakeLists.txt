# Lumen gigapixel stitcher: standalone build + acceptance tests.
# (The stitcher sources are also compiled directly into the `lumen` CLI by
# core/CMakeLists.txt; this project exists to build and run seam_test.)
cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(lumen_stitcher VERSION 0.1.0 LANGUAGES CXX)

add_executable(seam_test
        test/seam_test.cpp
        src/stitch.cpp
        src/bigtiff.cpp
        ../core/src/json.cpp)

target_include_directories(seam_test PRIVATE
        src
        third_party
        ../common
        ../core/src)

if (MSVC)
    target_compile_options(seam_test PRIVATE /W4 /permissive-)
else ()
    target_compile_options(seam_test PRIVATE -Wall -Wextra)
endif ()

if (MINGW)
    target_link_options(seam_test PRIVATE -static)
endif ()

enable_testing()
add_test(NAME seam_test COMMAND seam_test)
