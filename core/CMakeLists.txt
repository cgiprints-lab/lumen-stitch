cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(lumen VERSION 0.1.0 LANGUAGES CXX)

add_executable(lumen
        src/main.cpp
        src/json.cpp
        src/util.cpp
        src/adapter.cpp
        src/detect.cpp
        src/state.cpp)

if (MSVC)
    target_compile_options(lumen PRIVATE /W4 /permissive-)
else ()
    target_compile_options(lumen PRIVATE -Wall -Wextra)
endif ()

if (MINGW)
    # Ship without libstdc++/libgcc/winpthread DLLs next to the exe.
    target_link_options(lumen PRIVATE -static)
endif ()

# adapters/ lives one level above core/; resolve it relative to the source tree.
target_compile_definitions(lumen PRIVATE
        LUMEN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

install(TARGETS lumen RUNTIME DESTINATION bin)
